"""Compare settled uphill body support lag, using isolated runtime captures."""
import json
import math
from pathlib import Path

folder = Path(__file__).resolve().parents[1] / 'Saved/IKImplementation'

def metrics(name):
    rows = json.loads((folder / name).read_text())
    samples = [r for r in rows if r['phase'] == 'walk' and 4.6 < r['t'] < 5.9]
    assert len(samples) > 20
    gaps = []
    errors = []
    for r in samples:
        x, y, z = r['mesh_position']
        nx, ny, nz = r['normal']
        ox, oy, oz = r['plane_origin']
        ground_z = oz - (nx * (x-ox) + ny * (y-oy)) / nz
        gaps.append(ground_z - (z + r['pelvis'][2]))
        errors.extend(math.dist(f['actual'], f['target']) for f in r['feet'].values())
        assert r['pivot_speed'] == 15
    assert samples[-1]['mesh_position'][2] > samples[0]['mesh_position'][2] + 10
    return {'mean_body_support_lag_cm': sum(gaps)/len(gaps),
            'max_body_support_lag_cm': max(gaps), 'max_foot_target_error_cm': max(errors),
            'frames': len(samples)}

before = metrics('runtime-pelvis-before.json')
after = metrics('runtime-slope.json')
result = {'before': before, 'after': after}
print(json.dumps(result, indent=2))
assert after['mean_body_support_lag_cm'] < before['mean_body_support_lag_cm'] * .7
assert after['max_foot_target_error_cm'] < .5
(folder / 'pelvis-validation.json').write_text(json.dumps(result, indent=2))
