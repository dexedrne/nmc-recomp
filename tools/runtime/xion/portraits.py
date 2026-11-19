"""Run in Blender against X4's packed N_PO010_BTL.blend."""
from pathlib import Path
import bpy
from mathutils import Vector

scene=bpy.context.scene
scene.render.engine='CYCLES'
scene.cycles.device='CPU'
scene.cycles.samples=8
scene.render.film_transparent=True
scene.render.resolution_percentage=100
scene.render.resolution_x=256
scene.render.resolution_y=320
cam=scene.camera
for state,direction in enumerate(((.16,-1,.025),(-.22,-1,-.09),(.10,-1,.44))):
    direction=Vector(direction)
    cam.location=Vector((0,0,140))+direction*400
    cam.rotation_euler=(-direction).to_track_quat('-Z','Y').to_euler()
    cam.data.ortho_scale=36
    scene.render.filepath=str(Path.cwd()/'run-O56'/f'portrait-{state}.png')
    bpy.ops.render.render(write_still=True)
