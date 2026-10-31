import json, sqlite3

# 1) DB api keys
db = '/opt/astrbot/data/data_v4.db'
con = sqlite3.connect(db)
cur = con.cursor()
tabs = [r[0] for r in cur.execute("select name from sqlite_master where type='table'")]
print('tables:', tabs)
for t in tabs:
    if any(k in t.lower() for k in ('api', 'key', 'config')):
        try:
            cols = [c[1] for c in cur.execute('PRAGMA table_info(%s)' % t)]
            rows = cur.execute('select * from %s limit 5' % t).fetchall()
            print(t, 'cols=', cols)
            for r in rows[:5]:
                print('  ', str(r)[:160])
        except Exception as e:
            print(t, 'err', e)
con.close()

# 2) provider structure (masked)
cfg = json.load(open('/opt/astrbot/data/cmd_config.json', encoding='utf-8-sig'))
provs = cfg.get('provider', [])
print('provider count:', len(provs) if isinstance(provs, list) else type(provs))

def mask(o, depth=0):
    if isinstance(o, dict):
        return {k: mask(v, depth + 1) for k, v in o.items()}
    if isinstance(o, list):
        return [mask(i, depth + 1) for i in o]
    if isinstance(o, str) and len(o) > 10 and depth > 0:
        return o[:6] + '...'
    return o

if isinstance(provs, list):
    for i, p in enumerate(provs):
        print('provider[%d]:' % i, json.dumps(mask(p), ensure_ascii=False)[:300])

# 3) tts settings
tts = cfg.get('provider_tts_settings', {})
print('tts_settings:', json.dumps(mask(tts), ensure_ascii=False)[:500])

# 4) stt settings
stt = cfg.get('provider_stt_settings', {})
print('stt_settings:', json.dumps(mask(stt), ensure_ascii=False)[:300])
