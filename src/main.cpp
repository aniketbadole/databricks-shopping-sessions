#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <numeric>
#include <omp.h>
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
  for (int i = 0; i < n; i++) {
    auto p = s.find(',');
    if (i == n - 1) {
      if (p != SV::npos)
        return false;
      a[i] = s;
    } else {
      if (p == SV::npos)
        return false;
      a[i] = s.substr(0, p);
      s.remove_prefix(p + 1);
    }
    if (!field(a[i]))
      return false;
  }
  return true;
}
I days(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  int era = (y >= 0 ? y : y - 399) / 400;
  unsigned yo = y - era * 400;
  unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  return I(era) * 146097 + I(yo) * 365 + yo / 4 - yo / 100 + doy - 719468;
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
  if (!digits(s))
    return -1;
  I n = 0;
  for (char c : s) {
    int d = c - '0';
    if (n > (limit - d) / 10)
      return -1;
    n = n * 10 + d;
    if (n > limit)
      return -1;
  }
  return n;
}
I stamp(SV s) {
  if (digits(s))
    return number(s, numeric_limits<I>::max());
  if (s.size() != 20 && s.size() != 25)
    return -1;
  if (s[4] != '-' || s[7] != '-' || s[10] != 'T' || s[13] != ':' ||
      s[16] != ':')
    return -1;
  int y = number(s.substr(0, 4), 9999), m = number(s.substr(5, 2), 99),
      d = number(s.substr(8, 2), 99), h = number(s.substr(11, 2), 99),
      mi = number(s.substr(14, 2), 99), se = number(s.substr(17, 2), 99);
  if (y < 0 || m < 1 || m > 12 || d < 1 || h < 0 || h > 23 || mi < 0 ||
      mi > 59 || se < 0 || se > 59)
    return -1;
  static const int md[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  int maxd =
      md[m - 1] + (m == 2 && y % 4 == 0 && (y % 100 != 0 || y % 400 == 0));
  if (d > maxd)
    return -1;
  I offset = 0;
  if (s.size() == 20) {
    if (s[19] != 'Z')
      return -1;
  } else {
    if ((s[19] != '+' && s[19] != '-') || s[22] != ':')
      return -1;
    int oh = number(s.substr(20, 2), 99), om = number(s.substr(23, 2), 99);
    if (oh < 0 || oh > 23 || om < 0 || om > 59)
      return -1;
    offset = (oh * 60 + om) * 60000LL;
    if (s[19] == '-')
      offset = -offset;
  }
  return days(y, m, d) * DAY + (h * 3600 + mi * 60 + se) * 1000LL - offset;
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
struct DSU {
  Map<uint32_t> ids;
  vector<uint32_t> parent, sz;
  vector<SV> names;
  uint32_t add(SV s) {
    auto [it, newid] = ids.emplace(s, parent.size());
    if (newid) {
      parent.push_back(it->second);
      sz.push_back(1);
      names.push_back(s);
    }
    return it->second;
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
    for (uint32_t i = 0; i < parent.size(); i++)
      parent[i] = root(i);
  }
  SV person(SV s) const {
    auto it = ids.find(s);
    return it == ids.end() ? s : names[parent[it->second]];
  }
};
struct Event {
  SV user;
  I ts;
  uint32_t price, cat;
};
struct Click {
  SV user, id;
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
int main(int argc, char **argv) {
  try {
    if (argc != 5)
      throw runtime_error("usage: core DATA SCRATCH WINDOW_START WINDOW_END");
    auto started = chrono::steady_clock::now();
    auto log = [&](const char *s) {
      cerr << s << " "
           << chrono::duration<double>(chrono::steady_clock::now() - started)
                  .count()
           << "s\n";
    };
    filesystem::path data = argv[1], scratch = argv[2];
    filesystem::create_directories(scratch);
    I start = stoll(argv[3]), end = stoll(argv[4]);
    int threads = 8;
    if (auto p = getenv("NCPU"))
      threads = max(1, min(8, atoi(p)));
    omp_set_num_threads(threads);
    DSU dsu;
    for (auto f : files(data / "links"))
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
    using RawBuckets = array<vector<SV>, B>;
    vector<RawBuckets> raw(threads);
#pragma omp parallel for schedule(dynamic)
    for (size_t f = 0; f < ef.size(); f++) {
      auto &dest = raw[omp_get_thread_num()];
      lines(ef[f], [&](SV l) {
        auto pos = l.find(',');
        if (pos == SV::npos)
          return;
        SV id = l.substr(0, pos);
        if (!field(id) || id.empty())
          return;
        dest[Hash{}(id) % B].push_back(l);
      });
    }
    using EventBuckets = array<vector<Event>, B>;
    vector<EventBuckets> ev(threads);
#pragma omp parallel for schedule(dynamic)
    for (int b = 0; b < B; b++) {
      size_t n = 0;
      for (auto &r : raw)
        n += r[b].size();
      Map<Winner> keep;
      keep.reserve(n);
      for (auto &r : raw) {
        for (SV l : r[b]) {
          SV a[7];
          if (!parse(l, a, 7) || a[0].empty())
            continue;
          I ing = stamp(a[3]);
          auto [it, yes] = keep.emplace(a[0], Winner{l, ing});
          if (!yes && ing < it->second.ingest)
            it->second = {l, ing};
        }
        vector<SV>().swap(r[b]);
      }
      auto &dest = ev[omp_get_thread_num()];
      for (auto &[id, w] : keep) {
        SV a[7];
        parse(w.line, a, 7);
        if (a[1].empty() || a[5].empty())
          continue;
        I t = stamp(a[2]);
        if (t < start || t >= end || w.ingest - t > DAY)
          continue;
        I price = 0;
        if (a[4] == "purchase") {
          price = number(a[6], 10000000);
          if (price < 1)
            continue;
        } else if (a[4] != "view" && a[4] != "cart")
          continue;
        SV u = dsu.person(a[1]);
        uint32_t cat = 0;
        if (price) {
          auto it = products.find(a[5]);
          if (it != products.end())
            cat = it->second;
        }
        dest[Hash{}(u) % B].push_back({u, t, uint32_t(price), cat});
      }
    }
    raw.clear();
    log("events parsed/deduplicated");
    using ClickBuckets = array<vector<Click>, B>;
    vector<ClickBuckets> cl(threads);
#pragma omp parallel for schedule(dynamic)
    for (size_t f = 0; f < cf.size(); f++) {
      auto &dest = cl[omp_get_thread_num()];
      lines(cf[f], [&](SV l) {
        SV a[4];
        if (!parse(l, a, 4) || a[0].empty() || a[1].empty())
          return;
        auto it = campids.find(a[2]);
        if (it == campids.end())
          return;
        I t = stamp(a[3]);
        if (t < start - WEEK || t >= end)
          return;
        SV u = dsu.person(a[1]);
        dest[Hash{}(u) % B].push_back({u, a[0], t, it->second});
      });
    }
    log("clicks parsed");
    // Duplicate clicks are identical: their multiplicity cannot affect
    // last-touch or the set of active campaigns. Avoid an unnecessary global
    // click-ID dedup.
    array<Totals, B> totals;
#pragma omp parallel for schedule(dynamic)
    for (int b = 0; b < B; b++) {
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
      sort(e.begin(), e.end(), [](auto &a, auto &b) {
        return a.user < b.user || (a.user == b.user && a.ts < b.ts);
      });
      sort(c.begin(), c.end(), [](auto &a, auto &b) {
        if (a.user != b.user)
          return a.user < b.user;
        if (a.ts != b.ts)
          return a.ts < b.ts;
        return a.id > b.id;
      });
      ofstream out(scratch / ("user-" + to_string(b) + ".csv"));
      out << "user_id,day,sessions,events,revenue_cents,top_category\n";
      vector<uint32_t> count(campaigns.size(), 0);
      unordered_set<uint32_t> active;
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
          quoted(out, e[u].user);
          out << ',' << date(daily) << ',' << ns << ',' << nev << ',' << rev
              << ',';
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
            quoted(out, cats[top]);
          out << '\n';
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
            if (count[k]++ == 0)
              active.insert(k);
          }
          while (left < right && c[left].ts < t - WEEK) {
            uint32_t k = c[left++].campaign;
            if (--count[k] == 0)
              active.erase(k);
          }
          uint32_t credit = left < right ? c[right - 1].campaign : 0;
          auto &v = totals[b][key(credit, day)];
          v.sessions++;
          v.purchasing += revenue > 0;
          v.revenue += revenue;
          for (uint32_t k : active)
            totals[b][key(k, day)].assists++;
          s = se;
        }
        flush();
        for (auto k : active)
          count[k] = 0;
        active.clear();
        ci = ce;
        u = ue;
      }
      if (!out)
        throw runtime_error("write user CSV");
    }
    log("sessions/attribution aggregated");
    Totals total;
    for (auto &t : totals)
      for (auto &[k, v] : t)
        total[k] += v;
    ofstream out(scratch / "campaign.csv");
    out << "campaign_id,channel,day,sessions,purchasing_sessions,revenue_cents,"
           "assisted_sessions\n";
    for (auto &[k, v] : total) {
      uint32_t id = uint32_t(k);
      quoted(out, campaigns[id]);
      out << ',';
      quoted(out, channels[id]);
      out << ',' << date(int32_t(k >> 32)) << ',' << v.sessions << ','
          << v.purchasing << ',' << v.revenue << ',' << v.assists << '\n';
    }
    if (!out)
      throw runtime_error("write campaign CSV");
    log("core complete");
    return 0;
  } catch (exception &e) {
    cerr << e.what() << '\n';
    return 1;
  }
}
