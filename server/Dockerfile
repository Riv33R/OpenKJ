FROM python:3.12-slim

WORKDIR /app

RUN pip install --no-cache-dir mutagen

COPY remote_library_server.py /app/remote_library_server.py

ENV OPENKJ_HOST=0.0.0.0 \
    OPENKJ_PORT=8080 \
    OPENKJ_KARAOKE_DIR=/media/karaoke \
    OPENKJ_BREAK_DIR=/media/break

EXPOSE 8080

CMD ["python", "-u", "/app/remote_library_server.py"]
