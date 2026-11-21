FROM debian:bookworm-slim AS build
RUN apt-get update && apt-get install -y --no-install-recommends g++ && rm -rf /var/lib/apt/lists/*
COPY src/main.cpp /build/main.cpp
RUN g++ -O3 -DNDEBUG -std=c++17 -fopenmp /build/main.cpp -o /build/core

FROM python:3.12-slim-bookworm
RUN apt-get update && apt-get install -y --no-install-recommends libgomp1 && rm -rf /var/lib/apt/lists/* && useradd --uid 1000 --no-create-home --home-dir /scratch app
COPY requirements.txt /app/requirements.txt
RUN pip install --no-cache-dir -r /app/requirements.txt
COPY --from=build /build/core /app/core
COPY pipeline/ /app/pipeline/
COPY run.sh /app/run.sh
RUN chmod +x /app/run.sh
ENV PYTHONDONTWRITEBYTECODE=1 PYTHONUNBUFFERED=1 POLARS_MAX_THREADS=8
WORKDIR /app
USER 1000:1000
CMD ["/app/run.sh"]
