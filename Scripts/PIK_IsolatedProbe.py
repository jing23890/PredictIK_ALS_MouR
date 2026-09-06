import unreal, json, traceback
assert '-PIKIsolatedTest' in unreal.SystemLibrary.get_command_line(), 'Only run in the isolated test process'
level = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
level.editor_play_simulate()
phase = 'wait'
elapsed = 0.0
rows = []
report = unreal.Paths.project_saved_dir() + 'IKImplementation/runtime-step.json'
def vec(v): return [v.x,v.y,v.z]
def tick(dt):
    global phase, elapsed, character, anim
    try:
        if phase == 'wait':
            world=unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_game_world()
            if not world: return
            characters=unreal.GameplayStatics.get_all_actors_of_class(world,unreal.Character)
            character=next(c for c in characters if c.get_name()=='ALS_AnimMan_CharacterBP7_0')
            meshes=unreal.GameplayStatics.get_all_actors_of_class(world,unreal.StaticMeshActor)
            candidates=[a for a in meshes if 'MCP' in a.get_actor_label()]
            if not candidates:
                candidates=[a for a in meshes if (a.get_actor_location()-unreal.Vector(-1300,-800,50)).length()<5]
            assert candidates, 'Connection-test cube missing; do not repurpose arbitrary map geometry'
            cube=candidates[0]
            cube.static_mesh_component.set_mobility(unreal.ComponentMobility.MOVABLE)
            cube.set_actor_location(unreal.Vector(-1400,-390,10),False,True)
            cube.set_actor_scale3d(unreal.Vector(2,4,.2))
            character.set_actor_location(unreal.Vector(-2000,-390,94),False,True)
            character.set_actor_rotation(unreal.Rotator(0,0,0),True)
            character.set_editor_property('DesiredGait',type(character.get_editor_property('DesiredGait')).WALKING)
            character.character_movement.set_editor_property('run_physics_with_no_controller',True)
            character.mesh.set_editor_property('visibility_based_anim_tick_option',unreal.VisibilityBasedAnimTickOption.ALWAYS_TICK_POSE_AND_REFRESH_BONES)
            anim=character.mesh.get_anim_instance()
            assert isinstance(anim,unreal.PIKAnimInstance)
            phase='walk'
            return
        if phase == 'walk':
            elapsed+=dt
            character.add_movement_input(unreal.Vector(1,0,0),1,True)
            row={'t':elapsed,'position':vec(character.get_actor_location()),'alpha':anim.get_editor_property('PIK_Alpha'),
                 'status':anim.get_editor_property('PIK_Status'),'pelvis':vec(anim.get_editor_property('PIK_PelvisOffsetCS')),'feet':{}}
            for side in ['L','R']:
                s=anim.get_editor_property('PIK_FootState_'+side)
                row['feet'][side]={'planted':s.initialized and s.planted,'path_valid':s.path_valid,
                    'path':[vec(p) for p in s.path_points_ws],'target':vec(s.target_ankle_ws),
                    'actual':vec(character.mesh.get_socket_location('foot_'+side.lower())),
                    'anchor':vec(s.plant_contact_ws),'time_to_land':s.time_to_land_sec}
            rows.append(row)
            if elapsed>=8:
                with open(report,'w') as f: json.dump(rows,f)
                level.editor_request_end_play()
                phase='end'
        elif phase=='end' and not level.is_in_play_in_editor():
            unreal.unregister_slate_post_tick_callback(handle)
            unreal.SystemLibrary.quit_editor()
    except Exception:
        with open(report+'.error','w') as f: f.write(traceback.format_exc())
        unreal.log_error(traceback.format_exc())
        unreal.unregister_slate_post_tick_callback(handle)
        level.editor_request_end_play()
        unreal.SystemLibrary.quit_editor()
handle=unreal.register_slate_post_tick_callback(tick)
