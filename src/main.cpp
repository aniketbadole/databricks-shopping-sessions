#include <algorithm>
#include <array>
#include <arrow/api.h>
#include <arrow/io/api.h>
#include <chrono>
#include <cstring>
#include <emmintrin.h>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <mutex>
#include <numeric>
#include <omp.h>
#include <parquet/arrow/writer.h>
#include <regex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>
using namespace std;
using SV = string_view;
using I = int64_t;
constexpr int B = 64;
constexpr I DAY = 86400000, WEEK = 7 * DAY;
struct Hash {
  size_t operator()(SV s) const noexcept { return hash<SV>{}(s); }
};
template <class V> using Map = unordered_map<SV, V, Hash>;
struct Mapped {
  char *p = nullptr;
  size_t n = 0;
  explicit Mapped(const filesystem::path &path) {
    int f = open(path.c_str(), O_RDONLY);
    if (f < 0)
      throw runtime_error("open: " + path.string());
    struct stat st;
    if (fstat(f, &st))
      throw runtime_error("stat");
    n = st.st_size;
    if (n) {
      p = (char *)mmap(nullptr, n, PROT_READ, MAP_PRIVATE, f, 0);
      if (p == MAP_FAILED)
        throw runtime_error("mmap");
    }
    close(f);
  }
  ~Mapped() {
    if (p && p != MAP_FAILED)
      munmap(p, n);
  }
  Mapped(const Mapped &) = delete;
};
vector<unique_ptr<Mapped>> maps;
vector<SV> files(const filesystem::path &dir) {
  vector<filesystem::path> paths;
  for (auto &e : filesystem::directory_iterator(dir))
    if (e.path().extension() == ".csv")
      paths.push_back(e.path());
  sort(paths.begin(), paths.end());
  vector<SV> out;
  for (auto &p : paths) {
    maps.push_back(make_unique<Mapped>(p));
    auto &m = *maps.back();
    out.emplace_back(m.p, m.n);
  }
  return out;
}
SV file(const filesystem::path &p) {
  maps.push_back(make_unique<Mapped>(p));
  auto &m = *maps.back();
  return {m.p, m.n};
}
template <class F> void lines(SV s, F fn) {
  size_t pos = s.find('\n');
  if (pos == SV::npos)
    return;
  ++pos;
  while (pos < s.size()) {
    auto end = s.find('\n', pos);
    if (end == SV::npos)
      end = s.size();
    fn(s.substr(pos, end - pos));
    pos = end + 1;
  }
}
bool field(SV &s) {
  if (!s.empty() && s[0] == '"') {
    if (s.size() < 2 || s.back() != '"')
      return false;
    s = s.substr(1, s.size() - 2);
  }
  return s.find('"') == SV::npos;
}
bool parse(SV s, SV *a, int n) {
  size_t ends[8];
  int count = 0, quotes = 0;
  size_t i = 0;
  const __m128i comma = _mm_set1_epi8(','), quote = _mm_set1_epi8('"');
  for (; i + 16 <= s.size(); i += 16) {
    __m128i v =
        _mm_loadu_si128(reinterpret_cast<const __m128i *>(s.data() + i));
    unsigned c = _mm_movemask_epi8(_mm_cmpeq_epi8(v, comma));
    __m128i quoted_bytes =
        _mm_and_si128(_mm_cmpeq_epi8(v, quote), _mm_set1_epi8(1));
    __m128i sums = _mm_sad_epu8(quoted_bytes, _mm_setzero_si128());
    quotes += int(_mm_cvtsi128_si64(sums) +
                  _mm_cvtsi128_si64(_mm_srli_si128(sums, 8)));
    while (c) {
      if (count >= n - 1)
        return false;
      ends[count++] = i + __builtin_ctz(c);
      c &= c - 1;
    }
  }
  for (; i < s.size(); i++) {
    if (s[i] == '"')
      quotes++;
    if (s[i] == ',') {
      if (count >= n - 1)
        return false;
      ends[count++] = i;
    }
  }
  if (count != n - 1)
    return false;
  ends[count] = s.size();
  size_t begin = 0;
  int allowed = 0;
  for (int j = 0; j < n; j++) {
    a[j] = s.substr(begin, ends[j] - begin);
    begin = ends[j] + 1;
    if (!a[j].empty() && a[j][0] == '"') {
      if (a[j].size() < 2 || a[j].back() != '"')
        return false;
      a[j] = a[j].substr(1, a[j].size() - 2);
      allowed += 2;
    }
  }
  return quotes == allowed;
}

I days_slow(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  int era = (y >= 0 ? y : y - 399) / 400;
  unsigned yo = y - era * 400;
  unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  return I(era) * 146097 + I(yo) * 365 + yo / 4 - yo / 100 + doy - 719468;
}
I days(int y, unsigned m, unsigned d) {
  struct Month {
    uint32_t key = 0;
    I start = 0;
  };
  static thread_local array<Month, 64> cache{};
  uint32_t key = uint32_t(y) * 16 + m;
  auto &entry = cache[(uint32_t(y) * 12 + m) & 63];
  if (entry.key != key)
    entry = {key, days_slow(y, m, 1)};
  return entry.start + d - 1;
}
bool digits(SV s) {
  if (s.empty())
    return false;
  for (unsigned char c : s)
    if (c < '0' || c > '9')
      return false;
  return true;
}
I number(SV s, I limit) {
  if (s.empty())
    return -1;
  I n = 0, quotient = limit / 10, remainder = limit % 10;
  for (char c : s) {
    int d = c - '0';
    if (d < 0 || d > 9)
      return -1;
    if (n > quotient || (n == quotient && d > remainder))
      return -1;
    n = n * 10 + d;
    if (n > limit)
      return -1;
  }
  return n;
}
I stamp(SV s) {
  if ((s.size() != 20 && s.size() != 25) || s[4] != '-')
    return number(s, numeric_limits<I>::max());
  if (s[7] != '-' || s[10] != 'T' || s[13] != ':' || s[16] != ':')
    return -1;
  auto two = [&](int p) {
    unsigned a = unsigned(s[p] - '0'), b = unsigned(s[p + 1] - '0');
    return a > 9 || b > 9 ? -1 : int(a * 10 + b);
  };
  int y0 = two(0), y1 = two(2), m = two(5), d = two(8), h = two(11),
      mi = two(14), se = two(17);
  if (y0 < 0 || y1 < 0 || m < 1 || m > 12 || d < 1 || unsigned(h) > 23 ||
      unsigned(mi) > 59 || unsigned(se) > 59)
    return -1;
  int y = y0 * 100 + y1;
  static constexpr int md[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (d > md[m - 1] + (m == 2 && y % 4 == 0 && (y % 100 != 0 || y % 400 == 0)))
    return -1;
  I offset = 0;
  if (s.size() == 20) {
    if (s[19] != 'Z')
      return -1;
  } else {
    if ((s[19] != '+' && s[19] != '-') || s[22] != ':')
      return -1;
    int oh = two(20), om = two(23);
    if (unsigned(oh) > 23 || unsigned(om) > 59)
      return -1;
    offset = (oh * 60 + om) * 60000LL;
    if (s[19] == '-')
      offset = -offset;
  }
  return days(y, m, d) * DAY + (h * 3600 + mi * 60 + se) * 1000LL - offset;
}
// ingest_ts is guaranteed valid F1 by the input contract.
I ingest_stamp(SV s) {
  auto two = [&](int i) { return (s[i] - '0') * 10 + s[i + 1] - '0'; };
  int y = two(0) * 100 + two(2);
  return days(y, two(5), two(8)) * DAY +
         (two(11) * 3600 + two(14) * 60 + two(17)) * 1000LL;
}
string date(I day) {
  I z = day + 719468;
  I era = (z >= 0 ? z : z - 146096) / 146097;
  unsigned doe = z - era * 146097;
  unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  int y = yoe + era * 400;
  unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100),
           mp = (5 * doy + 2) / 153;
  unsigned d = doy - (153 * mp + 2) / 5 + 1, m = mp + (mp < 10 ? 3 : -9);
  y += m <= 2;
  char b[32];
  snprintf(b, sizeof(b), "%04d-%02u-%02u", y, m, d);
  return b;
}
void quoted(ostream &o, SV s) { o << '"' << s << '"'; }
struct IdCodec {
  Map<uint64_t> overflow;
  vector<SV> names;
  mutex guard;
  uint64_t encode(SV s) {
    uint64_t value = 0;
    if (s.size() <= 10) {
      static constexpr auto table = []() {
        array<unsigned char, 256> t{};
        for (int c = '0'; c <= '9'; c++)
          t[c] = c - '0' + 1;
        for (int c = 'A'; c <= 'Z'; c++)
          t[c] = c - 'A' + 11;
        for (int c = 'a'; c <= 'z'; c++)
          t[c] = c - 'a' + 37;
        t['_'] = 63;
        return t;
      }();
      unsigned invalid = 0;
      for (unsigned char c : s) {
        unsigned d = table[c];
        invalid |= (d == 0);
        value = (value << 6) | d;
      }
      if (invalid)
        value = 0;
      if (value)
        return value;
    }
    lock_guard<mutex> lock(guard);
    auto [it, yes] = overflow.emplace(s, (1ULL << 63) | names.size());
    if (yes)
      names.push_back(s);
    return it->second;
  }
  string name(uint64_t v) const {
    if (v >> 63)
      return string(names.at(v & ~(1ULL << 63)));
    char buf[10];
    int n = 0;
    while (v) {
      unsigned d = v & 63;
      buf[n++] = d <= 10   ? '0' + d - 1
                 : d <= 36 ? 'A' + d - 11
                 : d <= 62 ? 'a' + d - 37
                           : '_';
      v >>= 6;
    }
    reverse(buf, buf + n);
    return string(buf, n);
  }
} codec;
uint64_t mix(uint64_t x) {
  x ^= x >> 30;
  x *= 0xbf58476d1ce4e5b9ULL;
  x ^= x >> 27;
  x *= 0x94d049bb133111ebULL;
  return x ^ (x >> 31);
}
struct NumericMap {
  struct Slot {
    uint64_t key = 0;
    uint32_t value = 0;
  };
  vector<Slot> table = vector<Slot>(1024);
  size_t used = 0;
  void grow() {
    auto old = move(table);
    table.resize(old.size() * 2);
    used = 0;
    for (auto &s : old)
      if (s.key)
        insert(s.key, s.value);
  }
  pair<uint32_t *, bool> insert(uint64_t key, uint32_t value) {
    if ((used + 1) * 10 > table.size() * 7)
      grow();
    size_t p = mix(key) & (table.size() - 1);
    while (table[p].key && table[p].key != key)
      p = (p + 1) & (table.size() - 1);
    bool fresh = !table[p].key;
    if (fresh) {
      table[p] = {key, value};
      used++;
    }
    return {&table[p].value, fresh};
  }
  const uint32_t *find(uint64_t key) const {
    size_t p = mix(key) & (table.size() - 1);
    while (table[p].key) {
      if (table[p].key == key)
        return &table[p].value;
      p = (p + 1) & (table.size() - 1);
    }
    return nullptr;
  }
};
struct Person {
  uint64_t hash = 0;
  Person() = default;
  explicit Person(uint64_t v) : hash(v) {}
  bool operator==(const Person &b) const { return hash == b.hash; }
  bool operator!=(const Person &b) const { return hash != b.hash; }
  bool operator<(const Person &b) const { return hash < b.hash; }
};
struct DSU {
  NumericMap ids;
  vector<uint32_t> parent, sz;
  vector<SV> names;
  vector<uint64_t> canonical;
  uint32_t add(SV s) {
    auto [it, fresh] = ids.insert(codec.encode(s), parent.size());
    uint32_t id = *it;
    if (fresh) {
      parent.push_back(id);
      sz.push_back(1);
      names.push_back(s);
    }
    return id;
  }
  uint32_t root(uint32_t i) {
    while (parent[i] != i) {
      parent[i] = parent[parent[i]];
      i = parent[i];
    }
    return i;
  }
  void join(SV a, SV b) {
    auto x = root(add(a)), y = root(add(b));
    if (x == y)
      return;
    if (sz[x] < sz[y])
      swap(x, y);
    parent[y] = x;
    sz[x] += sz[y];
    names[x] = min(names[x], names[y]);
  }
  void finish() {
    canonical.resize(parent.size());
    for (uint32_t i = 0; i < parent.size(); i++) {
      parent[i] = root(i);
      canonical[i] = codec.encode(names[parent[i]]);
    }
  }
  Person person(SV s) const {
    uint64_t id = codec.encode(s);
    struct Entry {
      uint64_t key = 0, value = 0;
    };
    static thread_local array<Entry, 8192> cache{};
    auto &entry = cache[mix(id) & (cache.size() - 1)];
    if (entry.key == id)
      return Person(entry.value);
    auto it = ids.find(id);
    uint64_t value = it ? canonical[*it] : id;
    entry = {id, value};
    return Person(value);
  }
};
struct Event {
  Person user;
  I ts;
  uint32_t price, cat;
};
void sort_events(vector<Event> &events) {
  if (events.size() < 128) {
    sort(events.begin(), events.end(), [](const Event &a, const Event &b) {
      return a.user < b.user || (a.user == b.user && a.ts < b.ts);
    });
    return;
  }
  vector<Event> temp(events.size());
  I origin = events[0].ts;
  for (auto &e : events)
    origin = min(origin, e.ts);
  for (int column = 0; column < 2; column++)
    for (int shift = 0; shift < 64; shift += 8) {
      array<size_t, 256> count{};
      auto byte = [&](const Event &e) {
        return unsigned(
            ((column ? e.user.hash : uint64_t(e.ts - origin)) >> shift) & 255);
      };
      for (auto &e : events)
        count[byte(e)]++;
      if (*max_element(count.begin(), count.end()) == events.size())
        continue;
      size_t offset = 0;
      for (auto &n : count) {
        size_t old = n;
        n = offset;
        offset += old;
      }
      for (auto &e : events)
        temp[count[byte(e)]++] = e;
      events.swap(temp);
    }
}
struct Click {
  Person user;
  SV id;
  I ts;
  uint32_t campaign;
};
struct Winner {
  SV line;
  I ingest;
};
struct Counts {
  I sessions = 0, purchasing = 0, revenue = 0, assists = 0;
  Counts &operator+=(const Counts &b) {
    sessions += b.sessions;
    purchasing += b.purchasing;
    revenue += b.revenue;
    assists += b.assists;
    return *this;
  }
};
using Totals = unordered_map<uint64_t, Counts>;
uint64_t key(uint32_t campaign, I day) {
  return (uint64_t(uint32_t(day)) << 32) | campaign;
}
void check(const arrow::Status &s) {
  if (!s.ok())
    throw runtime_error(s.ToString());
}
template <class T> T result(arrow::Result<T> r) {
  if (!r.ok())
    throw runtime_error(r.status().ToString());
  return move(r).ValueOrDie();
}
shared_ptr<arrow::Array> finish(arrow::ArrayBuilder &b) {
  return result(b.Finish());
}
void parquet_write(const filesystem::path &path,
                   shared_ptr<arrow::Schema> schema,
                   vector<shared_ptr<arrow::Array>> arrays) {
  auto output = result(arrow::io::FileOutputStream::Open(path.string()));
  parquet::WriterProperties::Builder props;
  props.compression(parquet::Compression::SNAPPY);
  check(parquet::arrow::WriteTable(*arrow::Table::Make(schema, arrays),
                                   arrow::default_memory_pool(), output, 65536,
                                   props.build()));
  check(output->Close());
}
int main(int argc, char **argv) {
  try {
    if (argc != 3)
      throw runtime_error("usage: core DATA OUTPUT");
    auto started = chrono::steady_clock::now();
    auto log = [&](const char *s) {
      cerr << s << " "
           << chrono::duration<double>(chrono::steady_clock::now() - started)
                  .count()
           << "s\n";
    };
    filesystem::path data = argv[1], scratch = argv[2];
    filesystem::create_directories(scratch);
    ifstream manifest_file(data / "manifest.json");
    string manifest((istreambuf_iterator<char>(manifest_file)), {});
    auto window = [&](string key) {
      size_t p = manifest.find('"' + key + '"');
      if (p == string::npos)
        throw runtime_error("missing window key");
      p = manifest.find(':', p);
      p = manifest.find('"', p);
      size_t q = manifest.find('"', p + 1);
      if (p == string::npos || q == string::npos)
        throw runtime_error("invalid manifest");
      I t = stamp(SV(manifest).substr(p + 1, q - p - 1));
      if (t < 0)
        throw runtime_error("invalid window");
      return t;
    };
    I start = window("window_start"), end = window("window_end");
    filesystem::create_directories(scratch / "user_daily");
    filesystem::create_directories(scratch / "campaign_daily");
    int threads = 8;
    if (auto p = getenv("NCPU"))
      threads = max(1, min(8, atoi(p)));
    omp_set_num_threads(threads);
    DSU dsu;
    auto link_files = files(data / "links");
    size_t link_bytes = 0;
    for (auto f : link_files)
      link_bytes += f.size();
    size_t expected_ids = min<size_t>(link_bytes / 12 + 16, 10000000),
           table_size = 1024;
    while (table_size * 7 < expected_ids * 10)
      table_size *= 2;
    dsu.ids.table.resize(table_size);
    dsu.parent.reserve(expected_ids);
    dsu.sz.reserve(expected_ids);
    dsu.names.reserve(expected_ids);
    for (auto f : link_files)
      lines(f, [&](SV l) {
        SV a[2];
        if (parse(l, a, 2) && !a[0].empty() && !a[1].empty())
          dsu.join(a[0], a[1]);
      });
    dsu.finish();
    vector<SV> cats = {"unknown"}, campaigns = {"organic"},
               channels = {"organic"};
    Map<uint32_t> products, campids, catids;
    catids["unknown"] = 0;
    lines(file(data / "products.csv"), [&](SV l) {
      SV a[2];
      if (!parse(l, a, 2))
        throw runtime_error("products");
      auto [it, yes] = catids.emplace(a[1], cats.size());
      if (yes)
        cats.push_back(a[1]);
      products[a[0]] = it->second;
    });
    lines(file(data / "campaigns.csv"), [&](SV l) {
      SV a[2];
      if (!parse(l, a, 2))
        throw runtime_error("campaigns");
      campids[a[0]] = campaigns.size();
      campaigns.push_back(a[0]);
      channels.push_back(a[1]);
    });
    log("links/dimensions");
    // Split input into independent ID hash partitions. Full IDs are still
    // compared.
    vector<SV> ef = files(data / "events"), cf = files(data / "clicks");
    struct Candidate {
      uint64_t id;
      I ingest;
      Event event;
    };
    using RawBuckets = array<vector<Candidate>, B>;
    vector<RawBuckets> raw(threads);
    size_t input_bytes = 0;
    for (auto f : ef)
      input_bytes += f.size();
    size_t reserve_per_bucket =
        min<size_t>(input_bytes / 90, 100000000) / (threads * B);
    for (auto &worker : raw)
      for (auto &bucket : worker)
        bucket.reserve(reserve_per_bucket);
    // Divide on physical line boundaries, not files, to balance skewed shards.
    auto chunks = [](const vector<SV> &fs) {
      vector<SV> result;
      for (SV f : fs) {
        size_t begin = f.find('\n');
        if (begin == SV::npos)
          continue;
        ++begin;
        while (begin < f.size()) {
          size_t end = min(begin + 4 * 1024 * 1024, f.size());
          if (end < f.size()) {
            end = f.find('\n', end);
            if (end == SV::npos)
              end = f.size();
            else
              ++end;
          }
          result.push_back(f.substr(begin, end - begin));
          begin = end;
        }
      }
      return result;
    };
    auto work = chunks(ef);
#pragma omp parallel for schedule(dynamic)
    for (size_t f = 0; f < work.size(); f++) {
      auto &dest = raw[omp_get_thread_num()];
      SV input = work[f];
      while (!input.empty()) {
        size_t len = input.find('\n');
        if (len == SV::npos)
          len = input.size();
        SV l = input.substr(0, len);
        input.remove_prefix(min(len + 1, input.size()));
        SV a[7];
        if (!parse(l, a, 7) || a[0].empty())
          continue;
        I ing = ingest_stamp(a[3]);
        Candidate row{codec.encode(a[0]), ing, {{}, -1, 0, 0}};
        auto validate = [&]() {
          if (a[1].empty() || a[5].empty())
            return;
          I t = stamp(a[2]);
          if (t < start || t >= end || ing - t > DAY)
            return;
          I price = 0;
          if (a[4] == "purchase") {
            price = number(a[6], 10000000);
            if (price < 1)
              return;
          } else if (a[4] != "view" && a[4] != "cart")
            return;
          Person u = dsu.person(a[1]);
          uint32_t cat = 0;
          if (price) {
            auto it = products.find(a[5]);
            if (it != products.end())
              cat = it->second;
          }
          row.event = {u, t, uint32_t(price), cat};
        };
        validate();
        dest[mix(row.id) % B].push_back(row);
      }
    }
    log("events parsed");
    using EventBuckets = array<vector<Event>, B>;
    vector<EventBuckets> ev(threads);
#pragma omp parallel for schedule(dynamic)
    for (int b = 0; b < B; b++) {
      size_t n = 0;
      for (auto &r : raw)
        n += r[b].size();
      size_t capacity = 1;
      while (capacity < n * 2 + 1)
        capacity *= 2;
      // Table stores indices/pointers into stable candidate arrays, not copies.
      vector<const Candidate *> keep(capacity, nullptr);
      for (auto &r : raw)
        for (auto &row : r[b]) {
          size_t pos = (mix(row.id) / B) & (capacity - 1);
          while (keep[pos] && keep[pos]->id != row.id)
            pos = (pos + 1) & (capacity - 1);
          if (!keep[pos] || row.ingest < keep[pos]->ingest)
            keep[pos] = &row;
        }
      auto &dest = ev[omp_get_thread_num()];
      for (auto row : keep)
        if (row && row->event.ts != -1)
          dest[mix(row->event.user.hash) % B].push_back(row->event);
      for (auto &r : raw)
        vector<Candidate>().swap(r[b]);
    }
    raw.clear();
    log("events parsed/deduplicated");
    using ClickBuckets = array<vector<Click>, B>;
    vector<ClickBuckets> cl(threads);
    auto click_work = chunks(cf);
#pragma omp parallel for schedule(dynamic)
    for (size_t f = 0; f < click_work.size(); f++) {
      auto &dest = cl[omp_get_thread_num()];
      auto consume = [&](SV l) {
        SV a[4];
        if (!parse(l, a, 4) || a[0].empty() || a[1].empty())
          return;
        auto it = campids.find(a[2]);
        if (it == campids.end())
          return;
        I t = stamp(a[3]);
        if (t < start - WEEK || t >= end)
          return;
        Person u = dsu.person(a[1]);
        dest[mix(u.hash) % B].push_back({u, a[0], t, it->second});
      };
      SV input = click_work[f];
      while (!input.empty()) {
        size_t len = input.find('\n');
        if (len == SV::npos)
          len = input.size();
        consume(input.substr(0, len));
        input.remove_prefix(min(len + 1, input.size()));
      }
    }
    log("clicks parsed");
    // Duplicate clicks are identical: their multiplicity cannot affect
    // last-touch or the set of active campaigns. Avoid an unnecessary global
    // click-ID dedup.
    array<Totals, B> totals;
    I first_day = start / DAY, day_count = (end - 1) / DAY - first_day + 1;
    bool dense_mode =
        day_count > 0 && uint64_t(day_count) * campaigns.size() <= 250000;
    array<vector<Counts>, B> dense;
#pragma omp parallel for schedule(dynamic)
    for (int b = 0; b < B; b++) {
      if (dense_mode)
        dense[b].resize(size_t(day_count) * campaigns.size());
      auto campaign_total = [&](uint32_t id, I day) -> Counts & {
        if (dense_mode)
          return dense[b][size_t(day - first_day) * campaigns.size() + id];
        return totals[b][key(id, day)];
      };
      vector<Event> e;
      vector<Click> c;
      size_t ne = 0, nc = 0;
      for (auto &v : ev)
        ne += v[b].size();
      for (auto &v : cl)
        nc += v[b].size();
      e.reserve(ne);
      c.reserve(nc);
      for (auto &v : ev) {
        e.insert(e.end(), v[b].begin(), v[b].end());
        vector<Event>().swap(v[b]);
      }
      for (auto &v : cl) {
        c.insert(c.end(), v[b].begin(), v[b].end());
        vector<Click>().swap(v[b]);
      }
      sort_events(e);
      sort(c.begin(), c.end(), [](auto &a, auto &b) {
        if (a.user != b.user)
          return a.user < b.user;
        if (a.ts != b.ts)
          return a.ts < b.ts;
        return a.id > b.id;
      });
      arrow::StringBuilder out_user, out_category;
      arrow::Date32Builder out_day;
      arrow::Int64Builder out_sessions, out_events, out_revenue;
      vector<uint32_t> count(campaigns.size(), 0);
      vector<uint32_t> active, position(campaigns.size());
      active.reserve(campaigns.size());
      vector<I> category(cats.size(), 0);
      size_t ci = 0;
      for (size_t u = 0; u < e.size();) {
        size_t ue = u + 1;
        while (ue < e.size() && e[ue].user == e[u].user)
          ue++;
        while (ci < c.size() && c[ci].user < e[u].user)
          ci++;
        size_t ce = ci;
        while (ce < c.size() && c[ce].user == e[u].user)
          ce++;
        size_t left = ci, right = ci;
        I daily = -1, ns = 0, nev = 0, rev = 0;
        auto flush = [&]() {
          if (daily < 0)
            return;
          check(out_user.Append(codec.name(e[u].user.hash)));
          check(out_day.Append(daily));
          check(out_sessions.Append(ns));
          check(out_events.Append(nev));
          check(out_revenue.Append(rev));
          I best = 0;
          uint32_t top = 0;
          for (uint32_t k = 0; k < cats.size(); k++) {
            if (category[k] > best || (category[k] == best && category[k] > 0 &&
                                       cats[k] < cats[top])) {
              best = category[k];
              top = k;
            }
          }
          if (best)
            check(out_category.Append(cats[top]));
          else
            check(out_category.AppendNull());
        };
        for (size_t s = u; s < ue;) {
          size_t se = s + 1;
          while (se < ue && e[se].ts - e[se - 1].ts <= 1800000)
            se++;
          I t = e[s].ts, day = t / DAY;
          if (day != daily) {
            flush();
            daily = day;
            ns = nev = rev = 0;
            fill(category.begin(), category.end(), 0);
          }
          I revenue = 0;
          for (size_t j = s; j < se; j++) {
            revenue += e[j].price;
            if (e[j].price)
              category[e[j].cat] += e[j].price;
          }
          ns++;
          nev += se - s;
          rev += revenue;
          while (right < ce && c[right].ts <= t) {
            uint32_t k = c[right++].campaign;
            if (count[k]++ == 0) {
              position[k] = active.size();
              active.push_back(k);
            }
          }
          while (left < right && c[left].ts < t - WEEK) {
            uint32_t k = c[left++].campaign;
            if (--count[k] == 0) {
              uint32_t at = position[k], last = active.back();
              active[at] = last;
              position[last] = at;
              active.pop_back();
            }
          }
          uint32_t credit = left < right ? c[right - 1].campaign : 0;
          auto &v = campaign_total(credit, day);
          v.sessions++;
          v.purchasing += revenue > 0;
          v.revenue += revenue;
          for (uint32_t k : active)
            campaign_total(k, day).assists++;
          s = se;
        }
        flush();
        for (auto k : active)
          count[k] = 0;
        active.clear();
        ci = ce;
        u = ue;
      }
      parquet_write(
          scratch / "user_daily" / ("part-" + to_string(b) + ".parquet"),
          arrow::schema({arrow::field("user_id", arrow::utf8()),
                         arrow::field("day", arrow::date32()),
                         arrow::field("sessions", arrow::int64()),
                         arrow::field("events", arrow::int64()),
                         arrow::field("revenue_cents", arrow::int64()),
                         arrow::field("top_category", arrow::utf8())}),
          {finish(out_user), finish(out_day), finish(out_sessions),
           finish(out_events), finish(out_revenue), finish(out_category)});
    }
    log("sessions/attribution aggregated");
    Totals total;
    if (dense_mode)
      for (auto &part : dense)
        for (size_t i = 0; i < part.size(); i++) {
          const auto &v = part[i];
          if (v.sessions || v.assists)
            total[key(i % campaigns.size(),
                      first_day + i / campaigns.size())] += v;
        }
    for (auto &t : totals)
      for (auto &[k, v] : t)
        total[k] += v;
    arrow::StringBuilder out_campaign, out_channel;
    arrow::Date32Builder out_day;
    arrow::Int64Builder out_sessions, out_purchasing, out_revenue, out_assists;
    for (auto &[k, v] : total) {
      uint32_t id = uint32_t(k);
      check(out_campaign.Append(campaigns[id]));
      check(out_channel.Append(channels[id]));
      check(out_day.Append(int32_t(k >> 32)));
      check(out_sessions.Append(v.sessions));
      check(out_purchasing.Append(v.purchasing));
      check(out_revenue.Append(v.revenue));
      check(out_assists.Append(v.assists));
    }
    parquet_write(
        scratch / "campaign_daily" / "part-0.parquet",
        arrow::schema({arrow::field("campaign_id", arrow::utf8()),
                       arrow::field("channel", arrow::utf8()),
                       arrow::field("day", arrow::date32()),
                       arrow::field("sessions", arrow::int64()),
                       arrow::field("purchasing_sessions", arrow::int64()),
                       arrow::field("revenue_cents", arrow::int64()),
                       arrow::field("assisted_sessions", arrow::int64())}),
        {finish(out_campaign), finish(out_channel), finish(out_day),
         finish(out_sessions), finish(out_purchasing), finish(out_revenue),
         finish(out_assists)});
    log("core complete");
    return 0;
  } catch (exception &e) {
    cerr << e.what() << '\n';
    return 1;
  }
}
