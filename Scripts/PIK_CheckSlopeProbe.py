"""Check the completed isolated slope probe using only the Python standard library."""
import json, math, sys
from pathlib import Path
path=Path(__file__).resolve().parents[1]/'Saved/IKImplementation/runtime-slope.json'
if len(sys.argv)>1: path=Path(sys.argv[1])
rows=json.loads(path.read_text())
def mul(a,b):
    x,y,z,w=a; X,Y,Z,W=b
    return [w*X+x*W+y*Z-z*Y,w*Y-x*Z+y*W+z*X,w*Z+x*Y-y*X+z*W,w*W-x*X-y*Y-z*Z]
summary={}
for phase in ['stand','cross_slope','stop']:
    samples=[r for r in rows if r['phase']==phase][-30:]
    assert len(samples)==30,phase
    clearances=[]; angles=[]; errors=[]
    for r in samples:
        n=r['normal']; o=r['plane_origin']
        for side,f in r['feet'].items():
            clearances.append(sum((p-v)*a for p,v,a in zip(f['actual'],o,n))-13.46)
            errors.append(math.dist(f['actual'],f['target']))
            # Runtime consumes the previous completed reference pose, not the pose
            # rendered alongside this output (idle animation can rotate the feet).
            previous=rows[rows.index(r)-1]
            q=previous['feet'][side]['raw_rotation']; d=mul(f['rotation'],[-q[0],-q[1],-q[2],q[3]])
            up=mul(mul(d,[0,0,1,0]),[-d[0],-d[1],-d[2],d[3]])[:3]
            dot=sum(a*b for a,b in zip(up,n))
            angles.append(math.degrees(math.acos(max(-1,min(1,dot)))))
    summary[phase]={'min_alpha':min(r['alpha'] for r in samples),'max_sole_error_cm':max(map(abs,clearances)),
                    'max_target_error_cm':max(errors),'max_surface_angle_error_deg':max(angles)}
    assert summary[phase]['min_alpha']>=.99,summary[phase]
    assert summary[phase]['max_sole_error_cm']<.5,summary[phase]
    assert summary[phase]['max_target_error_cm']<.1,summary[phase]
    assert summary[phase]['max_surface_angle_error_deg']<3,summary[phase]
for side in ['L','R']:
    assert any(r['phase']=='walk' and r['feet'][side]['predicting'] for r in rows),side
    assert not any(r['phase']=='stop' and r['t']>6.5 and r['feet'][side]['predicting'] for r in rows),side
assert min(r['alpha'] for r in rows if r['phase']=='stop' and r['t']>6.5)>.99
assert any(r['phase']=='air' and r['alpha']<.01 for r in rows)
summary['frames']=len(rows)
summary['prediction_entry_and_stop_handoff']='passed'
summary['air_fade']='passed'
path.with_name(path.stem+'-validation.json').write_text(json.dumps(summary,indent=2))
print(json.dumps(summary,indent=2))
