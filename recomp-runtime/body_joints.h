#ifndef DOAXBV_RECOMP_BODY_JOINTS_H
#define DOAXBV_RECOMP_BODY_JOINTS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* How DOAXBV bends a girl's joint pieces (elbows, knees, ankles, wrists...)
   each frame, as plain maths on host data (docs/body-joints.md). Nothing
   here knows the guest: body_joints_adapter.c feeds it the game's records,
   and another game's model can feed it its own.

   Matrices are 16 floats, four rows (x, y, z axes, then the origin), and
   points multiply on the left (row vectors), as in the game. Every sum is
   in the game's order and precision, so the results are the game's to the
   bit: SSE helpers add in float, the x87 parts in double with float stores.

   A joint sits between two bones, a and b. `relative` is the matrix the
   joint's other side goes through; DOAXBV forms it per joint as bone b's
   entry in one of the girl's two matrix tables times bone a's entry in the
   other (her model's +0x1C and +0x20; recomp_joint_multiply). */

/* out = b * c (each row of b through c), in float:
   ((b0 * c0 + b1 * c1) + b2 * c2) + b3 * c3. The game's 0x00186FB5. */
void recomp_joint_multiply(float out[16], const float b[16], const float c[16]);

/* A direction through a matrix's first three rows, four lanes:
   (x * r0 + y * r1) + z * r2. The game's 0x0017B430. */
void recomp_joint_rotate(const float m[16], const float v[3], float out[4]);

/* A point through a matrix, four lanes:
   (x * r0 + y * r1) + (z * r2 + r3). The game's 0x0017B3F0. */
void recomp_joint_transform(const float m[16], const float p[3], float out[4]);

/* A vertex stored as a position (+0x00) and a normal (+0x10), 32 bytes,
   through a matrix: position ((x * r0 + y * r1) + z * r2) + r3, normal
   (x * r0 + y * r1) + z * r2. The game's 0x00067CA0. */
void recomp_joint_place(const float m[16], const float vertex[8], float position[4], float normal[4]);

/* sqrt(x) as the game takes it: a reciprocal square root refined by one
   Newton step, x * (1.5 * r - 0.5 * x * r * r * r), in float. */
float recomp_joint_sqrt(float x);

/* The per-joint frames (the game's scratch at 0x004CA500, four matrices):
   for each axis i < 3, row 0 the parent's axis a_i, row 1 the child's axis
   b_i through `relative`, row 2 their normalised sum (the halfway axis);
   then the origins: row 0 a point o0 as stored, row 1 o1 through
   `relative`, row 2 the midpoint of o2 through `relative` and o3 as
   stored. `data` is the joint's 40 floats (a_i, b_i, then o0..o3, four
   floats each). Rows the game leaves unwritten (row 3 of each) are left
   as they are. */
void recomp_joint_frames(const float relative[16], const float data[40], float frames[64]);

/* Interpolation weights between the parent (x), child (y) and halfway (z)
   frames at step k of 16 along the joint: the game's table at 0x00333770,
   a quadratic through t = 0, 1/2 and 1 (t = k / 16). */
void recomp_joint_weights(unsigned k, float weights[4]);

/* The frame at those weights: each of the four rows blended from the
   matching frame, then rows 0-2 made orthonormal (row 2 first, then row 1
   against it, then row 0 against both). `rows` gets 16 floats. The game
   leaves each rebuilt row's w lane holding a stale stack value; here it
   keeps the blend's w (the lanes are never read for vertices). */
void recomp_joint_blend_frame(const float frames[64], const float weights[4], float rows[16]);

#ifdef __cplusplus
}
#endif

#endif
