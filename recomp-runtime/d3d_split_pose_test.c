#include "d3d_split_pose.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#define CHECK(c) do { if (!(c)) { fprintf(stderr,"Split line %d: %s\n",__LINE__,#c); return 1; } } while (0)
static void identity(float m[16]) { memset(m,0,64); m[0]=m[5]=m[10]=m[15]=1; }
int main(void)
{
    CHECK(recomp_split_rate("100") == 100 && recomp_split_rate("144") == 144);
    CHECK(recomp_split_rate("120") == 120 && recomp_split_rate("240") == 240);
    CHECK(recomp_split_rate("119.88") == 119.88);
    CHECK(recomp_split_rate(NULL)==0 && recomp_split_rate("1")==0 && recomp_split_rate("nan")==0);
    CHECK(recomp_split_rate("120junk")==0 && recomp_split_rate("0")==0);
    const char *note;
    CHECK(recomp_split_requested("auto") && recomp_split_requested("144") && !recomp_split_requested(NULL));
    CHECK(recomp_split_display_rate("auto",143.981,true,&note)==143.981 && !note);
    CHECK(recomp_split_display_rate("auto",0,true,&note)==0 && note);
    CHECK(recomp_split_display_rate("auto",50,true,&note)==0 && note);
    CHECK(recomp_split_display_rate("120",119.88,true,&note)==119.88 && !note);
    CHECK(recomp_split_display_rate("240",144,true,&note)==144 && note);
    CHECK(recomp_split_display_rate("240",144,false,&note)==240 && note);
    CHECK(recomp_split_display_rate("100",144,true,&note)==100 && note);
    CHECK(recomp_split_display_rate("120",0,true,&note)==120 && !note);
    CHECK(recomp_split_display_rate(NULL,144,true,&note)==0 && !note);
    CHECK(!recomp_split_discontinuity(.21f,.13f,.01f) && !recomp_split_discontinuity(0,.07f,.01f));
    CHECK(recomp_split_discontinuity(.0015f,6.2f,.01f) && recomp_split_discontinuity(0,NAN,.01f));
    RecompVisualCamera camera[2] = {0};
    camera[0].eye[2] = 5; camera[0].fov = 1;
    camera[0].near_z = .1f; camera[0].far_z = 100;
    camera[0].scale_x=camera[0].scale_y=camera[0].scale_z=1; camera[0].aspect=2;
    camera[1]=camera[0]; camera[1].eye[0]=4; camera[1].target[0]=4;
    float view[16],projection[16];
    float motion[2]; recomp_split_camera_motion(camera,motion);
    CHECK(fabsf(motion[0]-.8f)<1e-6f && motion[1]==0);
    CHECK(recomp_animation_camera_sample(camera,.37f,view,projection));
    CHECK(fabsf(view[12]+1.48f)<1e-6f && fabsf(view[14]-5)<1e-6f);
    CHECK(fabsf(projection[0]*2-projection[5])<1e-6f && projection[11]==1);
    RecompVisualCamera steep=camera[0];
    steep.eye[0]=6.4f; steep.eye[1]=5; steep.eye[2]=-2.3f;
    steep.target[0]=1.1f; steep.target[1]=15.5f; steep.target[2]=5.1f;
    CHECK(recomp_animation_camera(&steep,view,projection));
    CHECK(fabsf(view[0]+.812975228f)<2e-6f);
    CHECK(fabsf(view[8]+.582295775f)<2e-6f);
    float saved[16]; memcpy(saved,view,64); camera[0].near_z=-1;
    CHECK(!recomp_animation_camera(camera,view,projection)); CHECK(memcmp(view,saved,64)==0);
    RecompSplitDraw split={0}; split.count=1; split.ball=1;
    identity(split.view); identity(split.projection); identity(split.worlds[0]); identity(split.ball_base.m);
    split.ball_position[1][0]=10;
    RecompD3dPresenterDrawCommand source={0}, output={0};
    source.has_transform=true; source.split_pose=&split; source.split_pose_size=sizeof split;
    CHECK(recomp_d3d_split_draw(&source,.37f,NULL,NULL,&output));
    CHECK(fabsf(output.transform[12]-3.7f)<1e-6f);
    CHECK(source.transform[12]==0 && output.pose_replay==NULL);
    RecompD3dPresenterDrawCommand before=output;
    CHECK(!recomp_d3d_split_draw(&source,1,NULL,NULL,&output)); CHECK(memcmp(&output,&before,sizeof output)==0);
    source.split_pose_size=sizeof split-1;
    CHECK(!recomp_d3d_split_draw(&source,.2f,NULL,NULL,&output));
    float from[16], to[16], mid[16];
    identity(from); identity(to);
    from[0]=from[5]=from[10]=2; from[12]=1;
    to[0]=to[10]=0; to[2]=-3; to[8]=3; to[5]=3; to[13]=4; /* 90 degrees about y, scale 3 */
    CHECK(recomp_split_rigid_matrix(from,to,0,mid));
    for(unsigned i=0;i<16;++i) CHECK(fabsf(mid[i]-from[i])<1e-6f);
    CHECK(recomp_split_rigid_matrix(from,to,1,mid));
    for(unsigned i=0;i<16;++i) CHECK(fabsf(mid[i]-to[i])<1e-6f);
    CHECK(recomp_split_rigid_matrix(from,to,.5f,mid));
    CHECK(fabsf(mid[0]-2.5f*cosf(.785398163f))<1e-5f && fabsf(mid[2]+2.5f*sinf(.785398163f))<1e-5f);
    CHECK(fabsf(mid[5]-2.5f)<1e-5f && fabsf(mid[12]-.5f)<1e-6f && fabsf(mid[13]-2)<1e-6f);
    to[1]=1; CHECK(!recomp_split_rigid_matrix(from,to,.5f,mid)); /* shear */
    to[1]=0; to[0]=-1; to[2]=0; to[8]=0; to[10]=1; CHECK(!recomp_split_rigid_matrix(from,to,.5f,mid)); /* mirror */
    identity(to); to[12]=8; split.ball=0; split.rigid=true;
    memcpy(split.rigid_worlds[0],split.worlds[0],64); memcpy(split.rigid_worlds[1],to,64);
    source.split_pose_size=sizeof split;
    CHECK(recomp_d3d_split_draw(&source,.25f,NULL,NULL,&output) && fabsf(output.transform[12]-2)<1e-6f);
    RecompVisualPose input={0}, unchanged;
    input.targets[0].terrain_disabled=1;
    const unsigned slots[4][4]={{3,5,4,6},{7,8,9,255},{12,11,10,13},{18,14,22,255}};
    for(unsigned i=0;i<4;++i){
        RecompLimbTable *d=input.tables.limbs+i; d->end=slots[i][0]; d->parent=2;
        d->proximal=slots[i][1];d->middle=slots[i][2];d->tip=slots[i][3];
        d->position_channels[0]=12;d->position_channels[1]=13;d->position_channels[2]=14;
        d->pole_channels[0]=15;d->pole_channels[1]=16;d->positive_bend=1;d->upper_body=i&1;
        input.targets[0].lengths[i].proximal=input.targets[0].lengths[i].distal=1;
        input.targets[0].lengths[i].alternate_proximal=input.targets[0].lengths[i].alternate_distal=1.1f;
    }
    input.channels[0].channels[14]=1.5f;input.channels[0].channels[15]=-1;
    input.channels[1]=input.channels[0];input.targets[1]=input.targets[0];input.targets[1].position[0]=10;
    for(unsigned i=0;i<2;++i)CHECK(recomp_animation_solve_skeleton(&input.tables,input.channels[i].channels,input.targets+i,input.endpoints[i]));
    for(unsigned i=0;i<24;++i) input.groups[i].width=3;
    unchanged=input;RecompBoneMatrix bones[32];
    CHECK(recomp_animation_visual_sample(&input,.37f,bones));
    CHECK(fabsf(bones[3].m[12]-input.endpoints[0][3].m[12]-3.7f)<1e-5f);
    CHECK(memcmp(&input,&unchanged,sizeof input)==0);
    CHECK(recomp_animation_visual_sample(&input,0,bones));CHECK(memcmp(bones,input.endpoints[0],sizeof bones)==0);
    CHECK(!recomp_animation_visual_sample(&input,NAN,bones));CHECK(memcmp(bones,input.endpoints[0],sizeof bones)==0);
    input.net_displacement[0]=.2f; input.net_displacement[1]=-.6f;
    float old_z;
    CHECK(recomp_animation_visual_sample(&unchanged,.37f,bones)); old_z=bones[3].m[14];
    CHECK(recomp_animation_visual_sample(&input,.37f,bones));
    CHECK(fabsf(bones[3].m[14]-old_z+.096f)<1e-6f);
    puts("Split arbitrary rate/fraction, camera, ball, immutable pose and validation tests passed");return 0;
}
