FROM python:3.12-slim-bookworm AS build
RUN apt-get update && apt-get install -y --no-install-recommends g++ && rm -rf /var/lib/apt/lists/*
RUN pip install --no-cache-dir pyarrow==26.0.1
COPY src/main.cpp /build/src/main.cpp
COPY scripts/build_native.py /build/scripts/build_native.py
RUN python /build/scripts/build_native.py && mkdir /arrow && python -c "import pathlib,shutil,pyarrow; p=pathlib.Path(pyarrow.get_library_dirs()[0]); [shutil.copy(f,'/arrow') for pattern in ('libarrow.so.*','libparquet.so.*') for f in p.glob(pattern)]"

FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends libgomp1 libstdc++6 && rm -rf /var/lib/apt/lists/* && useradd --uid 1000 --no-create-home --home-dir /scratch app
COPY --from=build /build/core /app/core
COPY --from=build /arrow/ /opt/arrow/
COPY run.sh /app/run.sh
RUN chmod +x /app/run.sh
ENV LD_LIBRARY_PATH=/opt/arrow
WORKDIR /app
USER 1000:1000
CMD ["/app/run.sh"]
