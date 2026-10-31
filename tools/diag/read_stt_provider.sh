#!/bin/bash
python3 - <<'EOF'
import json, re
cfg = json.load(open('/opt/astrbot/data/cmd_config.json', encoding='utf-8-sig'))
provs = cfg.get('provider', [])
print('total providers:', len(provs))
for i, p in enumerate(provs):
    pt = p.get('provider_type', '')
    if 'speech' in pt or 'stt' in str(p.get('id','')).lower() or 'whisper' in str(p.get('id','')).lower():
        s = json.dumps(p, ensure_ascii=False, indent=1)
        s = re.sub(r'("(?:api_key|api_base|key)":\s*")[^"]{14,}', r'\1<masked>', s)
        print('provider[%d] %s' % (i, s))
EOF
