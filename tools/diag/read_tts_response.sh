#!/bin/bash
echo "=====qwen_audio_tts.py (140+)====="
docker exec astrbot sh -lc 'sed -n "140,230p" /AstrBot/astrbot/core/provider/sources/qwen_audio_tts.py'
echo "=====mimo_stt_api_source.py head====="
docker exec astrbot sh -lc 'head -80 /AstrBot/astrbot/core/provider/sources/mimo_stt_api_source.py'
echo "=====provider[2] full====="
python3 - <<'EOF'
import json
cfg = json.load(open('/opt/astrbot/data/cmd_config.json', encoding='utf-8-sig'))
for p in cfg.get('provider', []):
    if p.get('provider_type') == 'speech_recognize':
        import re
        s = json.dumps(p, ensure_ascii=False)
        s = re.sub(r'("(?:api_key|api_base)":\s*")[^"]{12,}', r'\1<masked>', s)
        print(s)
EOF
