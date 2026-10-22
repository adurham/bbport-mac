import numpy as np, sys, json
from PIL import Image
H,W=480,1146
res={}
for tag,lay in [('a2','shifted'),('f1','fixed')]:
    g=[]
    for i in range(3,10):
        out=np.fromfile(f'dump-{tag}/f{i:03d}_output_3440x1440_rgba.raw',np.uint8).reshape(1440,3440,4)[...,:3].astype(np.float32)/255
        y=out@np.array([.299,.587,.114],np.float32)
        ob=np.fromfile(f'dump-{tag}/f{i:03d}_objects_1146x480_rgba32f.raw',np.float32).reshape(H,W,4)
        # character crop: fixed box around the centre-bottom (the hunter), same box both runs
        cy=y[600:1440,1350:2100]
        gx=np.abs(np.diff(cy,axis=1))[:-1]; gy=np.abs(np.diff(cy,axis=0))[:,:-1]; gm=np.hypot(gx,gy)
        g.append([gm.mean(),np.percentile(gm,99)])
        if i==6: Image.fromarray((np.clip(out[600:1440,1350:2100],0,1)*255).astype(np.uint8)).save(f'png-{"f1" if tag=="f1" else "a2"}/f006_fsr_output_character_crop.png')
    res[tag]=np.mean(g,0).round(5).tolist()
print(json.dumps(res))
