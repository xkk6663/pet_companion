#!/bin/bash
# 1) 验证 mao-pet 密钥调用 /api/v1/chat 是否仍有效
echo "=====chat key test====="
curl -s -N -X POST "http://127.0.0.1:6185/api/v1/chat" \
  -H "Authorization: Bearer abk_dnnS-QNxL31EOZQ6E6oTarxktEMEK04FJeJ7JuiQ3RA" \
  -H "Content-Type: application/json" \
  -d '{"username":"xiao1","session_id":"xz-s31-keytest","message":[{"type":"plain","text":"你好，回复两个字即可"}],"enable_streaming":true}' \
  --max-time 40 | head -c 1200
echo ""
echo "=====provider details====="
python3 - <<'EOF'
import json
cfg = json.load(open('/opt/astrbot/data/cmd_config.json', encoding='utf-8-sig'))
def mask(o, depth=0):
    if isinstance(o, dict): return {k: mask(v, depth+1) for k, v in o.items()}
    if isinstance(o, list): return [mask(i, depth+1) for i in o]
    if isinstance(o, str) and len(o) > 12 and depth > 0: return o[:8] + '...'
    return o
provs = cfg.get('provider', [])
for i, p in enumerate(provs):
    t = p.get('provider_type', '')
    if t in ('text_to_speech', 'speech_recognize') or 'audio' in str(p.get('id','')):
        print('provider[%d]:' % i, json.dumps(mask(p), ensure_ascii=False, indent=1))
EOF
