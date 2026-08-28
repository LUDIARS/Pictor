/// 影絵デモ — 切り絵シートのカットアウト (透かし穴) 定義。
///
/// シートは「光を全部遮る黒紙」で、ここで true を返した場所だけが
/// 切り抜き = 光が透過する。shadow map へ depth を書く側 (sp_depth_cut.frag)
/// と舞台裏の可視化 (sp_sheet.frag) の両方から include して、遮蔽と見た目を
/// 常に一致させる。
///
/// モチーフ (藤城清治風の切り絵を参照):
///   - 月の丸穴          … 障子に月の円盤が浮かぶ
///   - 平行光条の束        … 月光が細い遮蔽リブで分かれて差す
///   - 無数の葉状の小穴   … 梢の透かし。細かな光の粒が紙面に散る
///
/// 座標系: メッシュ uv (generate_screen_quad 準拠、y=1 が下辺) を
/// sp_cutout_hole() 内で y-up へ直してから判定する。円系モチーフは
/// シートの実寸アスペクト (kSheetW / kSheetH = 9.0 / 6.5) で補正する。
/// モチーフは月光ビームがシートを横切る帯 (おおよそ x 0.35-0.65,
/// y 0.39-0.62) に収めること — 帯の外は月フラスタムが通らず、切っても
/// 何も透けない。

#include "sp_shapes.glsl"

// main.cpp の kSheetW/kSheetH/kSheetY/kSheetZ と一致させる。
const float SP_SHEET_W = 9.0;
const float SP_SHEET_H = 6.5;
const float SP_SHEET_Y = 6.5;
const float SP_SHEET_Z = -9.5;
const float SP_SHEET_ASPECT = SP_SHEET_W / SP_SHEET_H;

// main.cpp の figure_model / kMeshFigure quad (1.1 x 1.5) と一致させる。
const vec3 SP_FIGURE_POS  = vec3(0.20, -0.40, -0.70);
const vec2 SP_FIGURE_SIZE = vec2(1.1, 1.5);

/// 月→ワールド点の光線が人影平面 (z = SP_FIGURE_POS.z) を横切る位置で
/// 人影シルエットに当たるか。shadow map を介さない解析判定なので、
/// 障子の直接透過・ゴッドレイ・水面反射のすべてで同じ影が出る。
bool sp_figure_blocks(vec3 world_pos, vec3 moon_pos) {
    float denom = world_pos.z - moon_pos.z;
    if (abs(denom) < 1e-4) return false;
    float t = (SP_FIGURE_POS.z - moon_pos.z) / denom;
    if (t <= 0.0 || t >= 1.0) return false;
    vec3 q = moon_pos + t * (world_pos - moon_pos);
    vec2 uv_up = (q.xy - SP_FIGURE_POS.xy) / SP_FIGURE_SIZE + 0.5;
    if (uv_up.x < 0.0 || uv_up.x > 1.0 || uv_up.y < 0.0 || uv_up.y > 1.0)
        return false;
    return sp_figure_solid(vec2(uv_up.x, 1.0 - uv_up.y)); // mesh uv (y下向き) へ
}

float sp_cut_hash12(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

/// 月光線に沿ってワールド点をシートへ戻し、y-up の判定座標を返す。
vec2 sp_sheet_hit(vec3 world_pos, vec3 moon_pos) {
    float t = (SP_SHEET_Z - moon_pos.z) / (world_pos.z - moon_pos.z);
    vec3 s = moon_pos + t * (world_pos - moon_pos);
    return vec2((s.x + SP_SHEET_W * 0.5) / SP_SHEET_W,
                (s.y - SP_SHEET_Y + SP_SHEET_H * 0.5) / SP_SHEET_H);
}

/// 月の丸穴の中心と半径 (y-up シート判定座標、アスペクト補正後の空間)。
/// sp_screen.frag / sp_godray.frag の鷹シルエット合成も同じ円を参照する。
/// 中心は「障子スクリーン上で左 1/4 (x≈-2.0)、全体が見える高さ (y≈1.2)」に
/// 月の円盤が射影される位置から逆算した値。
const vec2  SP_MOON_CENTER = vec2(0.44, 0.56);
const float SP_MOON_RADIUS = 0.026;

/// 月の丸穴に収めた鷹の切り絵 (Figmentum-kirie サンプル) の透過率。
/// 穴の外は 1 (素通し)。穴ローカル (-1..1) へ正規化し、鷹テクスチャを
/// 横=全幅、縦=画像アスペクト (1024x768) 維持でマッピングする。
/// 切り絵の紙は月光をほぼ遮り、わずかな暖色の透けだけを残す。
/// @implements SPEC-SHADOW-KIRIE-BACKDROP
vec3 sp_hawk_filter(vec2 sheet_p, sampler2D hawk_tex) {
    vec2 local = (sheet_p - SP_MOON_CENTER)
               * vec2(SP_SHEET_ASPECT, 1.0) / SP_MOON_RADIUS;
    if (dot(local, local) >= 1.0) return vec3(1.0);
    // 係数 1.25 = 画像の全幅が円盤直径の 1/1.25 ≈ 75% に縮まり、
    // 翼の先まで丸穴の内側へ収まる。縦は画像アスペクトを保つ。
    vec2 huv = vec2(0.5 + local.x * 0.5 * 1.25,
                    0.5 - local.y * 0.5 * 1.25 * (1024.0 / 768.0));
    vec4 hawk = texture(hawk_tex, clamp(huv, 0.0, 1.0));
    return mix(vec3(1.0), hawk.rgb * 0.05, hawk.a);
}

/// 月の丸穴。
bool sp_cut_moon_disc(vec2 p) {
    // 上端に明確な満月を置く。小さ過ぎる穴はスクリーンで点光源に見えるため、
    // 月として読める直径を確保する。
    vec2 d = (p - SP_MOON_CENTER) * vec2(SP_SHEET_ASPECT, 1.0);
    return length(d) < SP_MOON_RADIUS;
}

/// 月から扇形に開く主光芒。ホール (shadow map) は硬い輪郭のまま、
/// 見た目の柔らかさ・内部の光線ムラは sp_cutout_tint 側で作る。
/// 平行なスリット束は「白い板」に見えたため、月を頂点とする放射に変更。
const vec2  SP_BEAM_APEX   = SP_MOON_CENTER; // = 月の中心
const float SP_BEAM_TILT   = 0.70;   // 真下からの傾き (左の月から人物側へ)
const float SP_BEAM_SPREAD = 0.26;   // 片側の開き角 (rad)
const float SP_BEAM_TOP    = 0.530;  // 月の下端 (0.56 - 0.026) のすぐ下
const float SP_BEAM_BOTTOM = 0.335;

/// 月中心からの放射角。0 = 真下。
float sp_beam_angle(vec2 p) {
    vec2 v = (p - SP_BEAM_APEX) * vec2(SP_SHEET_ASPECT, 1.0);
    return atan(v.x, -v.y);
}

bool sp_cut_beam_slot(vec2 p) {
    if (p.y < SP_BEAM_BOTTOM || p.y > SP_BEAM_TOP) return false;
    return abs(sp_beam_angle(p) - SP_BEAM_TILT) < SP_BEAM_SPREAD;
}

/// 葉状の小穴の敷き詰め。セルごとの hash で有無・位置・大きさ・傾きを
/// 散らし、梢の帯の中だけに分布させる。
bool sp_cut_leaves_at(vec2 p, out vec2 cell_id) {
    cell_id = vec2(0.0);
    // 楕円の樹冠の外は判定しない
    vec2 crown_d = (p - vec2(0.50, 0.475)) * vec2(SP_SHEET_ASPECT, 1.0);
    if (length(crown_d) > 0.155) return false;
    // 主光芒の扇形の周囲は葉を間引いて光芒の輪郭を立てる
    if (p.y > SP_BEAM_BOTTOM && p.y < SP_BEAM_TOP &&
        abs(sp_beam_angle(p) - SP_BEAM_TILT) < SP_BEAM_SPREAD * 1.5) return false;

    const float kCell = 1.0 / 80.0;      // 葉 1 枚のセルサイズ
    cell_id = floor(p / kCell);
    float h = sp_cut_hash12(cell_id);
    if (h > 0.86) return false;          // 葉冠を細かな透かしで密に埋める

    // セル内で中心と形を散らす
    vec2 center = (cell_id + 0.5 + 0.5 * (vec2(sp_cut_hash12(cell_id + 7.3),
                                               sp_cut_hash12(cell_id + 13.1)) - 0.5)) * kCell;
    float radius = kCell * (0.20 + 0.18 * sp_cut_hash12(cell_id + 3.7));
    float ang    = 6.2832 * sp_cut_hash12(cell_id + 21.9);

    // 傾いた楕円 (葉) の内外判定
    vec2 d = (p - center) * vec2(SP_SHEET_ASPECT, 1.0);
    vec2 e = vec2( d.x * cos(ang) + d.y * sin(ang),
                  -d.x * sin(ang) + d.y * cos(ang));
    e.y *= 1.75;                         // セロファンの小葉らしい丸みを残す
    return length(e) < radius;
}

bool sp_cut_leaves(vec2 p) {
    vec2 cell_id;
    return sp_cut_leaves_at(p, cell_id);
}

/// カットアウトを通る月光のセロファン色。p は y-up のシート判定座標。
vec3 sp_cutout_tint(vec2 p) {
    if (sp_cut_moon_disc(p)) return vec3(1.0);

    if (sp_cut_beam_slot(p)) {
        float a = sp_beam_angle(p) - SP_BEAM_TILT;
        // 縁へ向かって減衰する柔らかいカーテン (ホールの硬い輪郭より内側で消える)
        float edge = 1.0 - smoothstep(SP_BEAM_SPREAD * 0.45, SP_BEAM_SPREAD, abs(a));
        // 月から放射する細い光線ムラ (角度方向の 1D 値ノイズ)
        float cell = floor(a * 260.0);
        float fr   = fract(a * 260.0);
        float rays = mix(sp_cut_hash12(vec2(cell, 3.7)),
                         sp_cut_hash12(vec2(cell + 1.0, 3.7)),
                         fr * fr * (3.0 - 2.0 * fr));
        float fade = mix(1.0, 0.80,
                         (SP_BEAM_TOP - p.y) / (SP_BEAM_TOP - SP_BEAM_BOTTOM));
        return vec3(edge * mix(0.70, 1.0, rays) * fade);
    }

    vec2 cell_id;
    if (!sp_cut_leaves_at(p, cell_id)) return vec3(1.0);

    const vec3 kPalette[6] = vec3[6](
        vec3(0.95, 0.25, 0.20), // 茜
        vec3(1.00, 0.62, 0.18), // 琥珀
        vec3(0.20, 0.80, 0.70), // 青緑
        vec3(0.85, 0.30, 0.80), // 紅紫
        vec3(0.30, 0.45, 0.95), // 藍
        vec3(0.55, 0.85, 0.30)  // 若草
    );
    int palette_index = int(floor(sp_cut_hash12(cell_id) * 6.0));
    return kPalette[palette_index] * 1.35;
}

/// カットアウト総合判定。true = 穴 (光が透過)。
bool sp_cutout_hole(vec2 mesh_uv) {
    vec2 p = vec2(mesh_uv.x, 1.0 - mesh_uv.y); // y-up へ
    if (sp_cut_moon_disc(p)) return true;
    if (sp_cut_beam_slot(p)) return true;
    if (sp_cut_leaves(p)) return true;
    return false;
}
