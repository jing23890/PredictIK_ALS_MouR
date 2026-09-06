"""Temporary PIE-only forward-walk probe; never saves or edits source assets."""
import unreal, json, traceback

world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_game_world()
assert world and 'UEDPIE_' in world.get_path_name(), 'Start Simulate before probing'
character = unreal.GameplayStatics.get_all_actors_of_class(world, unreal.Character)[-1]
anim = character.mesh.get_anim_instance()
assert isinstance(anim, unreal.PIKAnimInstance)
character.set_editor_property('DesiredGait', type(character.get_editor_property('DesiredGait')).WALKING)
character.character_movement.set_editor_property('run_physics_with_no_controller', True)
character.mesh.set_editor_property('visibility_based_anim_tick_option', unreal.VisibilityBasedAnimTickOption.ALWAYS_TICK_POSE_AND_REFRESH_BONES)
direction = character.get_actor_forward_vector()
rows = []
elapsed = 0.0
report = unreal.Paths.project_saved_dir() + 'IKImplementation/runtime-probe.json'

def vec(v): return [v.x,v.y,v.z]
def tick(dt):
    global elapsed, handle
    try:
        elapsed += dt
        character.add_movement_input(direction, 1.0, True)
        row = {'t':elapsed, 'position':vec(character.get_actor_location()), 'speed':character.get_velocity().length(),
               'alpha':anim.get_editor_property('PIK_Alpha'), 'status':anim.get_editor_property('PIK_Status'),
               'pelvis':vec(anim.get_editor_property('PIK_PelvisOffsetCS')), 'feet':{}}
        for side in ['L','R']:
            state = anim.get_editor_property('PIK_FootState_'+side)
            foot = character.mesh.get_socket_location('foot_'+side.lower())
            target = state.target_ankle_ws
            row['feet'][side] = {'planted':state.initialized and state.planted, 'path_valid':state.path_valid,
                'path': [vec(p) for p in state.path_points_ws], 'target':vec(target),'actual':vec(foot),
                'anchor':vec(state.plant_contact_ws), 'time_to_land':state.time_to_land_sec,
                'height_curve':anim.get_curve_value('FootHeight_'+side)}
        rows.append(row)
        if elapsed >= 8.0:
            unreal.unregister_slate_post_tick_callback(handle)
            character.character_movement.stop_movement_immediately()
            with open(report,'w',encoding='utf-8') as f: json.dump(rows,f)
            print('PIK probe complete:',len(rows),'frames;',report)
    except Exception:
        unreal.unregister_slate_post_tick_callback(handle)
        with open(report+'.error','w') as f: f.write(traceback.format_exc())
        unreal.log_error(traceback.format_exc())

handle = unreal.register_slate_post_tick_callback(tick)
print('PIK probe started:',character.get_name())
