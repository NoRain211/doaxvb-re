# SPDX-License-Identifier: GPL-3.0-or-later
"""Pure pose solver ported from the native skeleton model at 8ca957b.

Rig tables and frozen controller/terrain inputs are supplied, never inferred
from output bones. Float stores and approximate trig are part of the format.
"""
import math
import struct

from model_pose import identity


def f32(value):
    return struct.unpack('<f', struct.pack('<f', value))[0]


HALF_PI = f32(math.pi / 2)


def sine_cosine(angle):
    turns = f32(abs(angle) * f32(2 / f32(math.pi)))
    if turns >= 16777216:
        turns %= 4

    def quarter(turn):
        quadrant = int(turn)
        u = f32(turn - quadrant)
        if quadrant & 1:
            u = f32(1-u)
        square = f32(u*u)
        value = f32(-0.004681754135318688)
        for coefficient in (0.07969262624616703, -0.6459640975062462, math.pi/2):
            value = f32(f32(value*square) + f32(coefficient))
        value = f32(value*u)
        return -value if quadrant & 2 else value

    return math.copysign(1, angle)*quarter(turns), quarter(f32(turns+1))


def multiply(a, b):
    return [f32(sum(a[r*4+k]*b[k*4+c] for k in range(4)))
            for r in range(4) for c in range(4)]


def translate(matrix, xyz):
    out = matrix.copy()
    out[12:16] = [f32(sum(xyz[k]*matrix[k*4+c] for k in range(3)) + matrix[12+c])
                  for c in range(4)]
    return out


def rotate(matrix, axis, angle):
    rotation = identity()
    a, b = (axis+1) % 3, (axis+2) % 3
    s, c = sine_cosine(angle)
    rotation[a*4+a] = rotation[b*4+b] = c
    rotation[a*4+b], rotation[b*4+a] = s, -s
    return multiply(rotation, matrix)


def euler(matrix, angles):
    for axis in (2, 1, 0):
        matrix = rotate(matrix, axis, angles[axis])
    return matrix


def joint(parent, offset, angles):
    return euler(translate(parent, offset), angles)


def dot(a, b):
    return sum(a[i]*b[i] for i in range(3))


def normalize(vector):
    length = f32(math.sqrt(f32(dot(vector, vector))))
    return ([f32(v/length) for v in vector] if length else vector.copy()), length


def cross(a, b):
    return [f32(a[(i+1)%3]*b[(i+2)%3]-a[(i+2)%3]*b[(i+1)%3]) for i in range(3)]


def clamp(value, low=-1, high=1):
    return min(high, max(low, value))


def swing(a, b):
    cosine, axis = dot(a, b), cross(a, b)
    m = identity()
    if 1+cosine <= 1e-6:
        return m
    for r in range(3):
        for c in range(3):
            m[r*4+c] = f32((cosine if r == c else 0) + axis[r]*axis[c]/(1+cosine))
    for i in range(3):
        a, b = (i+1)%3, (i+2)%3
        m[a*4+b] = f32(m[a*4+b]+axis[i])
        m[b*4+a] = f32(m[b*4+a]-axis[i])
    return m


def blend(a, b, weight):
    transpose, rotation = identity(), identity()
    for r in range(3):
        for c in range(3):
            transpose[r*4+c] = a[c*4+r]
    relative = multiply(b, transpose)
    axis, length = normalize([f32(relative[i]-relative[j]) for i, j in ((6,9),(8,2),(1,4))])
    cosine = f32((relative[10]+relative[5]+relative[0]-1)*0.5)
    s, c = sine_cosine(f32(math.atan2(length*0.5, cosine)*weight))
    for r in range(3):
        for col in range(3):
            rotation[r*4+col] = f32((c if r == col else 0)+(1-c)*axis[r]*axis[col])
    for i in range(3):
        u, v = (i+1)%3, (i+2)%3
        rotation[u*4+v] = f32(rotation[u*4+v]+f32(s*axis[i]))
        rotation[v*4+u] = f32(rotation[v*4+u]-f32(s*axis[i]))
    out = multiply(rotation, a)
    out[12:15] = [f32((1-weight)*a[i]+weight*b[i]) for i in range(12,15)]
    return out


def derived_joints(bones, extra):
    for out, parent, child in ((26,16,8),(27,20,14),(24,5,4),(25,11,10)):
        bones[out] = multiply(bones[parent], swing(bones[parent], bones[child]))
        bones[out][12:16] = bones[child][12:16]
    for out, a, b in ((30,8,26),(31,14,27),(4,4,24),(10,10,25)):
        bones[out] = blend(bones[a], bones[b], 0.5)
    for out, source, location in ((24,4,5),(25,10,11)):
        bones[out] = blend(bones[source], bones[out], 0.5) if extra else bones[source].copy()
        bones[out+4] = bones[out].copy()
        bones[out+4][12:16] = bones[location][12:16]


def solve_limb(t, p, s, index, b):
    d, lengths = t['limbs'][index], s['lengths'][index]
    end, parent, proximal, middle, tip = d[:5]
    target = translate(b[15], [p[i] for i in d[5:8]])
    height = s['position'][1] if s['terrain_disabled'] else s['limb_height'][index]
    target[13] = f32(target[13]+height-s['position'][1])
    target[13] = f32(target[13]+s['position'][1]-s['ground_height'])
    if d[20] & s['target_mask']:
        target[12:15] = [f32(target[12+i]+s['target_weight']*s['target_offset'][i]) for i in range(3)]
    sa, ca = sine_cosine(p[d[11]])
    sz, cz = sine_cosine(p[d[12]])
    pole = [f32(ca*sz), -sa, f32(ca*cz)]
    world_pole = [f32(sum(pole[k]*b[15][k*4+i] for k in range(3))) for i in range(3)]
    weight = s['upper_morph'] if d[22] else s['lower_morph']

    def offset(i):
        return [f32(f32(1-weight)*v+weight*w)
                for v, w in zip(t['offsets'][i], t['alternate_offsets'][i])]

    anchor = b[parent]
    if weight > 0:
        anchor = b[2]
        if d[22]:
            anchor = joint(anchor, offset(23), p[44:47])
            anchor = translate(joint(anchor, offset(0), p[6:9]), offset(20))
    anchor = translate(anchor, offset(proximal) if weight > 0 else t['offsets'][proximal])
    direction = [f32(target[12+i]-anchor[12+i]) for i in range(3)]
    distance_squared = f32(dot(direction, direction))
    forward, distance = normalize(direction)
    projection = dot(world_pole, forward)
    up, up_length = normalize([f32(world_pole[i]-projection*forward[i]) for i in range(3)])
    if not distance or not up_length:
        raise ValueError('Singular limb target or pole')
    basis = identity()
    basis[:3], basis[4:7], basis[8:11] = cross(up, forward), up, forward
    basis[12:15] = translate(b[parent], t['offsets'][proximal])[12:15]
    first, second, difference = [f32(f32(1-weight)*lengths[k]+weight*lengths['alternate_'+k])
                                 for k in ('proximal','distal','squared_difference')]
    if first <= 0 or second <= 0:
        raise ValueError('Nonpositive limb length')
    alpha = beta = HALF_PI
    if distance > f32(0.001):
        alpha = f32(math.acos(clamp(f32((distance_squared+difference)/(2*distance*first)))))
        beta = f32(math.acos(clamp(f32((distance_squared-difference)/(2*distance*second)))))
    if s['minimum_bend']:
        alpha, beta = max(f32(0.1), alpha), max(f32(0.1), beta)
    if not d[21]:
        alpha, beta = -alpha, -beta
    basis = rotate(basis, 1, alpha)
    quarters = [v if v < 128 else v-256 for v in d[14:20]]
    b[proximal] = euler(basis, [f32(v*HALF_PI) for v in quarters[:3]])
    basis = rotate(translate(basis, [0,0,lengths['proximal']]), 1, f32(-beta-alpha))
    b[middle] = euler(basis, [f32(v*HALF_PI) for v in quarters[3:]])
    basis = translate(basis, [0,0,lengths['distal']])
    basis[:12] = b[15][:12]
    b[end] = euler(basis, [p[i] for i in d[8:11]])
    if tip != 255:
        b[tip] = rotate(translate(b[end], t['offsets'][tip]), 0, p[d[13]])


def correct_terrain(t, s, b):
    height, clearance = f32(s['neck_height']-s['hip_height']), f32(b[2][13]-s['hip_height'])
    if s['terrain_disabled'] or not height or clearance >= 0.5 or clearance <= f32(-0.1):
        return
    neck = translate(b[0], t['offsets'][19])
    horizontal, distance = normalize([f32(neck[12]-b[2][12]),0,f32(neck[14]-b[2][14])])
    if distance < f32(0.1):
        return
    weight = f32(1-f32(2*clearance))
    if distance < f32(0.2):
        weight = f32(weight*f32(f32(distance-f32(0.1))*10))
    slope, _ = normalize([horizontal[0],f32(height*clamp(weight,0,1)),horizontal[2]])
    correction = translate(swing(horizontal, slope), [-v for v in b[2][12:15]])
    correction[12:15] = [f32(correction[12+i]+b[2][12+i]) for i in range(3)]
    for i in (2,23,0):
        b[i] = multiply(b[i], correction)


def rotation_angles(m):
    horizontal = f32(math.sqrt(f32(m[6]*m[6]+m[10]*m[10])))
    if horizontal > 1e-8:
        return [f32(math.atan2(a,b)) for a,b in ((m[6],m[10]),(-m[2],horizontal),(m[1],m[0]))]
    return [f32(math.atan2(-m[4] if m[2]>0 else m[4],m[5])), -HALF_PI if m[2]>0 else HALF_PI, 0]


def look_angles(t, s, body, p):
    neck, head = p[47:50], p[9:12]
    if s['look_weight'] <= 0 or not all(math.isfinite(v) for v in s['look_target']):
        return neck, head
    delta = [f32(s['look_target'][i]-body[12+i]) for i in range(3)]
    local = [f32(dot(delta,body[i*4:i*4+3])-t['offsets'][19][i]-t['offsets'][1][i]) for i in range(3)]
    x,y,z = f32(local[0]*0.5),f32(local[1]*0.5),f32(f32(max(local[2],f32(0.05))+1)*0.5)
    pitch = clamp(f32(math.atan2(-y,f32(math.sqrt(f32(x*x+z*z))))),f32(-1.0471976),f32(0.17453294))
    yaw = clamp(f32(math.atan2(x,z)),f32(-1.0471976),f32(1.0471976))
    return tuple(rotation_angles(blend(euler(identity(),angles),
                 euler(identity(),[f32(pitch*f32(w)),f32(yaw*f32(w)),0]),s['look_weight']))
                 for angles,w in ((neck,0.3),(head,0.7)))


def solve_skeleton(tables, channels, targets):
    """Return 32 matrices, leaving inputs unchanged; reject malformed input."""
    t, s = tables, targets
    if len(channels)!=60 or any(not math.isfinite(v) or abs(v)>1e6 for v in channels):
        raise ValueError('Expected sixty bounded finite channels')
    p = [f32(v) for v in channels]
    for name in ('offsets','alternate_offsets'):
        if len(t[name])!=24 or any(len(row)<3 or not all(math.isfinite(v) for v in row) for row in t[name]):
            raise ValueError('Invalid rig offsets')
    if len(t['limbs'])!=4 or len(s['lengths'])!=4 or len(s['limb_height'])!=4:
        raise ValueError('Expected four limb records')
    for d in t['limbs']:
        if (len(d)!=24 or any(not isinstance(v,int) or not 0<=v<=255 for v in d)
                or any(d[i]>=32 for i in (0,1,3)) or d[2]>=24
                or (d[4]!=255 and d[4]>=24) or any(v>=60 for v in d[5:14])):
            raise ValueError('Invalid limb descriptor')
    def finite(value):
        if isinstance(value,dict):
            return all(finite(v) for v in value.values())
        if isinstance(value,(list,tuple)):
            return all(finite(v) for v in value)
        return isinstance(value,(float,int)) and math.isfinite(value)
    # The native look-at path treats a nonfinite look target as disabled.
    if (not finite({k:v for k,v in s.items() if k != 'look_target'})
            or abs(s['heading'])>1e6 or any(abs(v)>1e6 for v in s['eyes'])):
        raise ValueError('Invalid frozen targets')
    if any(not 0<=s[k]<=1 for k in ('upper_morph','lower_morph','look_weight')):
        raise ValueError('Invalid controller weight')
    for key,size in (('position',3),('target_offset',3),('look_target',3),('eyes',4)):
        if len(s[key]) != size:
            raise ValueError('Invalid target vector shape')
    if any(not isinstance(v,(float,int)) for v in s['look_target']):
        raise ValueError('Invalid look target')
    def rounded(value):
        if isinstance(value,dict):
            return {k:rounded(v) for k,v in value.items()}
        if isinstance(value,(list,tuple)):
            return [rounded(v) for v in value]
        return f32(value)
    flags = ('target_mask','minimum_bend','terrain_disabled','derived_blend')
    if any(not isinstance(s[k],int) or not 0<=s[k]<=255 for k in flags):
        raise ValueError('Invalid target flags')
    s = {k:v if k in flags else rounded(v) for k,v in s.items()}
    t = dict(t,offsets=rounded(t['offsets']),alternate_offsets=rounded(t['alternate_offsets']))
    b = [identity() for _ in range(32)]
    b[15] = rotate(translate(identity(),s['position']),1,s['heading'])
    b[2] = joint(b[15],[0,p[1],0],p[3:6])
    b[23] = joint(b[2],t['offsets'][23],p[44:47])
    b[0] = joint(b[23],t['offsets'][0],p[6:9])
    correct_terrain(t,s,b)
    for i,c in ((20,50),(16,52)):
        b[i] = rotate(rotate(translate(b[0],t['offsets'][i]),1,p[c+1]),2,p[c])
    neck,head = look_angles(t,s,b[0],p)
    b[19] = joint(b[0],t['offsets'][19],neck)
    b[1] = joint(b[19],t['offsets'][1],head)
    for i,c in ((21,0),(17,2)):
        b[i] = rotate(rotate(translate(b[1],t['offsets'][i]),1,s['eyes'][c+1]),0,s['eyes'][c])
    for i in range(4):
        solve_limb(t,p,s,i,b)
    derived_joints(b,s['derived_blend'])
    if not finite(b):
        raise ValueError('Nonfinite skeleton result')
    return b


def height_at(grid, x, z):
    """Bilinear scene height from an explicitly supplied private grid."""
    if not math.isfinite(x) or not math.isfinite(z) or grid['cell']<=0:
        raise ValueError('Invalid terrain query')
    if not grid['xmin']<=x<=grid['xmax'] or not grid['zmin']<=z<=grid['zmax']:
        return grid['base']
    inverse = 1/grid['cell']
    ix,iz = int((x-grid['xmin'])*inverse),int((z-grid['zmin'])*inverse)
    if not 0<=ix<grid['width']-1 or not 0<=iz<grid['stride']-1:
        raise ValueError('Terrain query lacks a complete grid cell')
    fx = f32((x-(ix*grid['cell']+grid['xmin']))*inverse)
    fz = (z-(iz*grid['cell']+grid['zmin']))*inverse
    one_x = f32(1-fx)
    at = ix*grid['stride']+iz
    h00,h01,h10,h11 = [grid['heights'][i]&4095 for i in (at,at+1,at+grid['stride'],at+grid['stride']+1)]
    return f32((h11*fz*fx+h10*(1-fz)*fx+fz*one_x*h01+(1-fz)*one_x*h00)/1024+grid['base'])


def solve_scene(tables, channels, targets, grid):
    """Query terrain using independent provisional bones, then solve once more."""
    s = dict(targets, hip_height=0.0, neck_height=0.0, limb_height=[0.0]*4)
    s['ground_height'] = height_at(grid,*[s['position'][i] for i in (0,2)])
    provisional = solve_skeleton(tables,channels,s)
    hip = provisional[2]
    neck = translate(provisional[0],tables['offsets'][19])
    s['hip_height'] = height_at(grid,hip[12],hip[14])
    s['neck_height'] = height_at(grid,neck[12],neck[14])
    for i,d in enumerate(tables['limbs']):
        point = translate(provisional[15],[f32(channels[k]) for k in d[5:8]])
        s['limb_height'][i] = height_at(grid,point[12],point[14])
    return solve_skeleton(tables,channels,s), s
