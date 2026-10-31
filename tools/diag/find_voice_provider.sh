#!/bin/bash
echo "=====find tts provider code====="
docker exec astrbot sh -lc 'grep -rln "qwen-audio\|qwen_audio\|text_to_speech" /AstrBot/astrbot --include="*.py" 2>/dev/null | head -10'
echo "=====find stt provider code====="
docker exec astrbot sh -lc 'grep -rln "speech_recognize\|audio/transcriptions\|FunAudioASR" /AstrBot/astrbot --include="*.py" 2>/dev/null | head -10'
