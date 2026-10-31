FROM python:3.12-slim

ENV PYTHONUNBUFFERED=1 \
    PYTHONDONTWRITEBYTECODE=1

WORKDIR /app
COPY music_proxy.py probe.py /app/

RUN useradd -r -u 10001 music && chown -R music /app
USER music

EXPOSE 8080
HEALTHCHECK --interval=30s --timeout=5s --start-period=5s --retries=3 \
  CMD python3 -c "import urllib.request;urllib.request.urlopen('http://127.0.0.1:8080/healthz',timeout=4).read()" || exit 1

CMD ["python3", "/app/music_proxy.py"]
