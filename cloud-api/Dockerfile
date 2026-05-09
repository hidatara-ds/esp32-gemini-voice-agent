# Use Python 3.11 slim image
FROM python:3.11-slim

WORKDIR /app

# System deps for audio processing
RUN apt-get update && apt-get install -y ffmpeg && rm -rf /var/lib/apt/lists/*

# Python deps (cached layer)
COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

# Application code
COPY cloud ./cloud
COPY static ./static
COPY app.py ./app.py

ENV PORT=8080
ENV PYTHONUNBUFFERED=1

EXPOSE 8080

# gthread worker — compatible with threading async_mode + gRPC Google libs
# workers=1 to avoid shared-memory issues with in-process session store
# threads=8 to handle concurrent WebSocket connections + background audio tasks
CMD ["sh", "-c", "gunicorn --bind 0.0.0.0:${PORT:-8080} --workers 1 --worker-class gthread --threads 8 --timeout 0 --keep-alive 75 cloud.main:app"]
