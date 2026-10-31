#!/bin/bash
echo "=====qwen_audio_tts.py====="
docker exec astrbot sh -lc 'cat /AstrBot/astrbot/core/provider/sources/qwen_audio_tts.py 2>/dev/null | head -140'
echo "=====stt sources====="
docker exec astrbot sh -lc 'ls /AstrBot/astrbot/core/provider/sources/ | grep -i stt'
echo "=====xinference_stt_provider.py====="
docker exec astrbot sh -lc 'cat /AstrBot/astrbot/core/provider/sources/xinference_stt_provider.py 2>/dev/null | head -90'
