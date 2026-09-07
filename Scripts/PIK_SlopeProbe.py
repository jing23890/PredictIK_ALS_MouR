"""Isolated PIE slope/stop-start regression; never saves the map."""
import unreal, json, traceback
assert '-PIKIsolatedTest' in unreal.SystemLibrary.get_command_line()
level=unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
level.editor_play_simulate()
phase='wait'
elapsed=0.0
rows=[]
report=unreal.Paths.project_saved_dir()+'IKImplementation/runtime-slope.json'
if '-PIKLandingBaseline' in unreal.SystemLibrary.get_command_line():
    report=unreal.Paths.project_saved_dir()+'IKImplementation/runtime-landing-before.json'
slope_angle=0 if '-PIKFlatTest' in unreal.SystemLibrary.get_command_line() else 20
if slope_angle==0:
    report=unreal.Paths.project_saved_dir()+'IKImplementation/runtime-flat-contact.json'
def vec(v): return [v.x,v.y,v.z]
def quat(q): return [q.x,q.y,q.z,q.w]
def tick(dt):
    global phase,elapsed,character,anim,cube,normal,origin
    try:
        if phase=='wait':
            world=unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_game_world()
            if not world: return
            character=next(c for c in unreal.GameplayStatics.get_all_actors_of_class(world,unreal.Character) if c.get_name()=='ALS_AnimMan_CharacterBP7_0')
            meshes=unreal.GameplayStatics.get_all_actors_of_class(world,unreal.StaticMeshActor)
            candidates=[a for a in meshes if 'MCP' in a.get_actor_label()]
            if not candidates: candidates=[a for a in meshes if (a.get_actor_location()-unreal.Vector(-1300,-800,50)).length()<5]
            assert candidates,'Test cube missing'
            cube=candidates[0]
            cube.static_mesh_component.set_mobility(unreal.ComponentMobility.MOVABLE)
            cube.set_actor_scale3d(unreal.Vector(12,12,.2))
            cube.set_actor_rotation(unreal.Rotator(pitch=slope_angle,yaw=0,roll=0),True)
            cube.set_actor_location(unreal.Vector(-1400,-390,100),False,True)
            normal=cube.get_actor_up_vector()
            origin=cube.get_actor_location()+normal*10
            character.set_actor_location(unreal.Vector(-1400,-390,240),False,True)
            character.set_actor_rotation(unreal.Rotator(0,0,0),True)
            character.set_editor_property('DesiredGait',type(character.get_editor_property('DesiredGait')).WALKING)
            character.character_movement.set_editor_property('run_physics_with_no_controller',True)
            character.mesh.set_editor_property('visibility_based_anim_tick_option',unreal.VisibilityBasedAnimTickOption.ALWAYS_TICK_POSE_AND_REFRESH_BONES)
            anim=character.mesh.get_anim_instance()
            phase='stand'
            return
        if phase=='end':
            if not level.is_in_play_in_editor():
                unreal.unregister_slate_post_tick_callback(handle)
                unreal.SystemLibrary.quit_editor()
            return
        elapsed+=dt
        if elapsed>=2 and phase=='stand':
            cube.set_actor_rotation(unreal.Rotator(pitch=slope_angle,yaw=90,roll=0),True)
            normal=cube.get_actor_up_vector()
            origin=cube.get_actor_location()+normal*10
            phase='cross_slope'
        if elapsed>=4 and phase=='cross_slope':
            cube.set_actor_rotation(unreal.Rotator(pitch=slope_angle,yaw=0,roll=0),True)
            normal=cube.get_actor_up_vector()
            origin=cube.get_actor_location()+normal*10
            character.set_actor_rotation(unreal.Rotator(0,0,0),True)
            phase='walk'
        if phase=='walk': character.add_movement_input(unreal.Vector(1,0,0),1,True)
        if elapsed>=6 and phase=='walk':
            character.character_movement.stop_movement_immediately()
            phase='stop'
        if elapsed>=8.5 and phase=='stop':
            character.launch_character(unreal.Vector(0,0,400),True,True)
            phase='air'
        row={'t':elapsed,'phase':phase,'normal':vec(normal),'plane_origin':vec(origin),
             'alpha':anim.get_editor_property('PIK_Alpha'),'status':anim.get_editor_property('PIK_Status'),
             'position':vec(character.get_actor_location()),'pelvis':vec(anim.get_editor_property('PIK_PelvisOffsetCS')),'feet':{}}
        row['mesh_position']=vec(character.mesh.get_world_location())
        row['pivot_speed']=anim.get_editor_property('PIK_PelvisInterpSpeed')
        for side in ['L','R']:
            s=anim.get_editor_property('PIK_FootState_'+side)
            foot=character.mesh.get_socket_transform('foot_'+side.lower(),unreal.RelativeTransformSpace.RTS_WORLD)
            raw=character.mesh.get_socket_transform('ik_foot_'+side.lower(),unreal.RelativeTransformSpace.RTS_WORLD)
            row['feet'][side]={'predicting':s.using_prediction,'pending':s.exit_prediction_pending,
                'planted':s.planted,'target':vec(s.target_ankle_ws),'actual':vec(foot.translation),
                'rotation':quat(foot.rotation),'raw_rotation':quat(raw.rotation),'contact_normal':vec(s.contact_normal_ws)}
            row['feet'][side].update({'raw_position':vec(raw.translation),'anchor':vec(s.plant_contact_ws),
                'height_curve':anim.get_curve_value('FootHeight_'+side),'path_progress':s.path_progress,
                'pelvis_actual':vec(character.mesh.get_socket_location('pelvis')),
                'time_to_land':s.time_to_land_sec})
        rows.append(row)
        if elapsed>=9.5:
            with open(report,'w') as f: json.dump(rows,f)
            level.editor_request_end_play()
            phase='end'
    except Exception:
        with open(report+'.error','w') as f: f.write(traceback.format_exc())
        unreal.log_error(traceback.format_exc())
        unreal.unregister_slate_post_tick_callback(handle)
        level.editor_request_end_play()
        unreal.SystemLibrary.quit_editor()
handle=unreal.register_slate_post_tick_callback(tick)
