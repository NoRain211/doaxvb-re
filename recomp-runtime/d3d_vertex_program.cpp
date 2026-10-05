#include "d3d_vertex_program.h"

namespace {
std::string mask(uint32_t bits)
{
    std::string s;
    for (unsigned i = 0; i < 4; ++i) if (bits & (8u >> i)) s += "xyzw"[i];
    return s;
}

std::string source(uint32_t mux, uint32_t reg, uint32_t swizzle,
                   bool negate, uint32_t attribute, uint32_t constant)
{
    std::string s;
    if (mux == 1 && reg <= 12) s = "r" + std::to_string(reg);
    else if (mux == 2 && (attribute == 0 || attribute == 2 || attribute == 9))
        s = "v" + std::to_string(attribute);
    else if (mux == 3 && constant < 192) s = "vc[" + std::to_string(constant) + "]";
    else return {};
    s += ".";
    for (unsigned i = 0; i < 4; ++i) s += "xyzw"[(swizzle >> (6u-2u*i)) & 3u];
    return negate ? "(-" + s + ")" : s;
}

bool write(std::string &s, const std::string &dest, uint32_t bits, const char *value)
{
    if (!bits) return true;
    if (dest.empty()) return false;
    const auto m = mask(bits);
    s += dest + "." + m + " = " + value + "." + m + ";\n";
    return true;
}
}

bool recomp_d3d_vertex_program_source(
    const uint32_t (*tokens)[4], uint32_t count, std::string &body)
{
    body.clear();
    if (!tokens || !count || count > 136) return false;
    std::string s = "float4 v0=float4(input.position,1), v2=float4(input.normal,1), v9=float4(input.texcoord,0,1);\n";
    for (unsigned i=0;i<13;++i) s += "float4 r"+std::to_string(i)+"=0;\n";
    s += "float4 o3=0,o5=0,o9=0,o10=0;\n";
    for (uint32_t i=0;i<count;++i) {
        const auto a=tokens[i][1], b=tokens[i][2], c=tokens[i][3];
        const auto mac=(a>>21)&15u, ilu=(a>>25)&7u;
        const auto attr=(a>>9)&15u, constant=(a>>13)&255u;
        if ((c&2u) || bool(c&1u)!=(i+1==count)) return false;
        auto x=source((b>>26)&3u,(b>>28)&15u,a&255u,(a&256u)!=0,attr,constant);
        auto y=source((b>>11)&3u,(b>>13)&15u,(b>>17)&255u,(b&(1u<<25))!=0,attr,constant);
        auto z=source((c>>28)&3u,((b&3u)<<2)|(c>>30),(b>>2)&255u,(b&1024u)!=0,attr,constant);
        std::string m="0", l="0";
        if (mac) {
            if (x.empty()) return false;
            if ((mac==2 || mac==4 || mac==5 || mac==7) && y.empty()) return false;
            if ((mac==3 || mac==4) && z.empty()) return false;
            switch(mac) {
            case 1:m=x;break;
            case 2:m=x+"*"+y;break;
            case 3:m=x+"+"+z;break;
            case 4:m=x+"*"+y+"+"+z;break;
            case 5:m="dot("+x+".xyz,"+y+".xyz).xxxx";break;
            case 7:m="dot("+x+","+y+").xxxx";break;
            default:return false;
            }
        }
        if (ilu) {
            if (z.empty()) return false;
            switch(ilu) {
            case 1:l=z;break;
            case 2:l="(1.0/("+z+").x).xxxx";break;
            case 3:l="((("+z+").x<0?-1.0:1.0)*clamp(abs(1.0/("+z+").x),5.42101e-20,1.884467e19)).xxxx";break;
            case 4:l="rsqrt(abs(("+z+").x)).xxxx";break;
            default:return false;
            }
        }
        // Both units read the old register values before either writes.
        s += "{float4 m="+m+", l="+l+";\n";
        const auto reg=(c>>20)&15u;
        if ((!mac && ((c>>24)&15u)) || (!ilu && ((c>>16)&15u))) return false;
        if (reg>12 && (((c>>24)&15u) || ((c>>16)&15u))) return false;
        // Paired ILU owns r1; suppress the MAC temporary write regardless of masks.
        if (!write(s,"r"+std::to_string(reg),ilu && reg==1u ? 0u : (c>>24)&15u,"m") ||
            !write(s,"r"+std::to_string(mac ? 1u : reg),(c>>16)&15u,"l")) return false;
        const auto om=(c>>12)&15u, out=(c>>3)&255u;
        if (om) {
            if (!(c&2048u)) return false; // Constant writes need a separate API contract.
            std::string dest=out==0 ? "r12" : (out==3 || out==5 || out==9 || out==10) ? "o"+std::to_string(out) : "";
            if (!write(s,dest,om,(c&4u)?"l":"m")) return false;
        }
        s += "}\n";
    }
    s += "output.position=mul(float4(r12.xyz,1),wvp[0])*r12.w;\n"
         "output.fog_depth=o5.x; output.fog_factor=o5.x;\n"
         "output.color=o3; output.texcoord=o9.xy; output.reflection_coord=o10.xy; output.program_q=float2(o9.w,o10.w);\n";
    body=std::move(s);
    return true;
}
