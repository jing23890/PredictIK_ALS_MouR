"""Compare real-time landing samples, including the frames around contact (not only the event)."""
import json, math
from pathlib import Path
root=Path(__file__).resolve().parents[1]/'Saved/IKImplementation'
def examine(name):
    rows=json.loads((root/name).read_text())
    events=[]
    for i in range(1,len(rows)-3):
        a,b=rows[i-1],rows[i]
        if b['phase']!='walk' or min(a['alpha'],b['alpha'])<.99: continue
        for side in ['L','R']:
            old,new=a['feet'][side],b['feet'][side]
            if not (old['predicting'] and new['predicting'] and not old['planted'] and new['planted']): continue
            window=[r for r in rows if b['t']-.08<=r['t']<=b['t']+.05]
            steps=[math.dist(x['feet'][side]['target'],y['feet'][side]['target']) for x,y in zip(window,window[1:])]
            speeds=[math.dist(x['feet'][side]['target'],y['feet'][side]['target'])/(y['t']-x['t']) for x,y in zip(window,window[1:])]
            events.append({'t':b['t'],'side':side,'landing_step_cm':math.dist(old['target'],new['target']),
                'raw_step_cm':math.dist(old['raw_position'],new['raw_position']),
                'completed_pose_step_cm':math.dist(rows[max(0,i-2)]['feet'][side]['raw_position'],old['raw_position']),
                'nearby_max_step_cm':max(steps),'nearby_max_speed_cm_sec':max(speeds),'actual_target_error_cm':math.dist(new['actual'],new['target']),
                'support_drift_cm':math.dist(new['target'],rows[i+1]['feet'][side]['target']) if rows[i+1]['feet'][side]['planted'] else None})
    assert len(events)>=2,'Insufficient normal forward-walk landings'
    return events
before=examine('runtime-landing-before.json')
after=examine('runtime-slope.json')
# The corrected solver consumes the last COMPLETED pose. Compare against that input's motion,
# not the newly rendered pose sampled after this update (which is one evaluation later).
assert all(e['landing_step_cm']<e['completed_pose_step_cm']+.5 for e in after),after
assert all(e['actual_target_error_cm']<.1 for e in after),after
assert all(e['support_drift_cm'] is None or e['support_drift_cm']<.1 for e in after),after
assert max(e['nearby_max_speed_cm_sec'] for e in after)<max(e['nearby_max_speed_cm_sec'] for e in before),(before,after)
result={'before':before,'after':after,'checks':'passed'}
if (root/'runtime-flat-contact.json').exists():
    flat=examine('runtime-flat-contact.json')
    assert all(e['landing_step_cm']<e['completed_pose_step_cm']+.5 for e in flat),flat
    assert all(e['actual_target_error_cm']<.1 and (e['support_drift_cm'] is None or e['support_drift_cm']<.1) for e in flat),flat
    result['flat']=flat
(root/'landing-validation.json').write_text(json.dumps(result,indent=2))
print(json.dumps(result,indent=2))
