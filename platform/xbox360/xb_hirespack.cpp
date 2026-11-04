// LAIKA 64 - HD texture packs downloaded from the internet (see xb_hirespack.h).

#include <xtl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "../../core/common/h64_log.h"
#include "../../core/common/h64_png.h"
#include "../../third_party/zlib/zlib.h"
#include "xb_net.h"
#include "xb_hirespack.h"
#include "xb_ui.h"

#include "7z.h"
#include "7zCrc.h"
#include "LzmaDec.h"
#include "Lzma2Dec.h"

// The packs there are, by the ROM header's name each was made for (the folder
// names of their repositories' texture sources); a game may have several.
static const HiresPackEntry s_packs[] = {
    { "Banjo-Kazooie", "BK-Reloaded", "GhostlyDark", "BK Reloaded", "GhostlyDark", NULL, 0, "https://raw.githubusercontent.com/GhostlyDark/BK-Reloaded/HEAD/bk-reloaded.jpg" },
    { "BANJO TOOIE", "BT-Reloaded", "GhostlyDark", "BT Reloaded", "GhostlyDark", NULL, 0, "https://raw.githubusercontent.com/GhostlyDark/BT-Reloaded/HEAD/bt-reloaded.jpg" },
    { "GOLDENEYE", "GoldenEye-007-HD", "GhostlyDark", "GoldenEye 007 HD", "intermissionfb", NULL, 0, "https://raw.githubusercontent.com/GhostlyDark/GoldenEye-007-HD/HEAD/ge007-hd.jpg" },
    { "MARIOKART64", "MK64-Reloaded", "GhostlyDark", "MK64 Reloaded", "GhostlyDark", NULL, 0, "https://raw.githubusercontent.com/GhostlyDark/MK64-Reloaded/HEAD/mk64-reloaded.jpg" },
    { "Resident Evil II", "RE2-Reloaded", "GhostlyDark", "RE2 Reloaded", "GhostlyDark", NULL, 0, "https://raw.githubusercontent.com/GhostlyDark/RE2-Reloaded/HEAD/re2-reloaded.jpg" },
    { "SMASH BROTHERS", "SSB-Reloaded", "GhostlyDark", "SSB Reloaded", "GhostlyDark", NULL, 0, "https://raw.githubusercontent.com/GhostlyDark/SSB-Reloaded/HEAD/ssb-reloaded.jpg" },
    { "SUPER MARIO 64", "SM64-Reloaded", "GhostlyDark", "SM64 Reloaded", "GhostlyDark", NULL, 0, "https://raw.githubusercontent.com/GhostlyDark/SM64-Reloaded/HEAD/sm64-reloaded.jpg" },
    { "SUPER MARIO 64", "Mario-Craft-64", "GhostlyDark", "Mario Craft 64", "Hypatia", NULL, 0, "https://raw.githubusercontent.com/GhostlyDark/Mario-Craft-64/HEAD/mario-craft-64.jpg" },
    { "THE LEGEND OF ZELDA", "OoT-Reloaded", "GhostlyDark", "OoT Reloaded", "GhostlyDark", NULL, 0, "https://raw.githubusercontent.com/GhostlyDark/OoT-Reloaded/HEAD/oot-reloaded.jpg" },
    { "Turok 3- Shadow of O", "Turok3-Reloaded", "GhostlyDark", "Turok 3 Reloaded", "GhostlyDark", NULL, 0, "https://raw.githubusercontent.com/GhostlyDark/Turok3-Reloaded/HEAD/turok3-reloaded.jpg" },
    { "F-ZERO X", "F-Zero-X-HQ-Pilot-Portrait-Texture-Pack", "DotZDRA", "F-Zero X HQ Portraits", "DotZDRA", "Portraits.1080p.zip", 0, NULL },
    // Smash Remix Ultimate 5.0, its 480p pack (a .hts in a .zip; 720p and HD are larger): archive.org.
    { "SMASH REMIX", "https://archive.org/download/sru-5.0-hd/SRU-5.0-480p.zip", "", "Smash Remix Ultimate 480p", "Hxc Conker", NULL, 433284213u,
      "https://archive.org/download/sru-5.0-hd/5.0.jpg" },
    // ... which the original Super Smash Bros. takes too (the same textures).
    { "SMASH BROTHERS", "https://archive.org/download/sru-5.0-hd/SRU-5.0-480p.zip", "", "Smash Remix Ultimate 480p", "Hxc Conker", NULL, 433284213u,
      "https://archive.org/download/sru-5.0-hd/5.0.jpg" },
    // Direct downloads (owner ""): the community's PNG packs (Rice's format) kept by Emulation King
    // (emulationking.com/nintendo/n64/texturepacks) and n64textures.com, each its URL and size.
    { "Banjo-Kazooie", "https://files.emulationking.com/n64/texturepacks/banjokazooie/Banjo_Kazooie_Macphisto.zip", "", "Mad Monster Mansion HD", "Macphisto", NULL, 47664197u, "https://emulationking.com/nintendo/n64/games/banjokazooie/texturepacks/macphisto/Banjo-Kazooie-macphisto-Texture-Pack-Coverpic_hu_bf6fd32d30cf1ef3.jpg" },
    { "Banjo-Kazooie", "https://files.emulationking.com/n64/texturepacks/banjokazooie/Banjo-Kazooie-by-nikachu.zip", "", "Painty Retexture", "Nikachu", NULL, 519723273u, "https://emulationking.com/nintendo/n64/games/banjokazooie/texturepacks/nikachu/nikachu-banjo-kazooie-texture-pack_hu_66cd3b7de2a126a0.png" },
    { "Banjo-Kazooie", "https://files.emulationking.com/n64/texturepacks/banjokazooie/banjo-Kazooie_the_nameless.zip", "", "Retexture", "The Nameless", NULL, 1855548u, "https://emulationking.com/nintendo/n64/games/banjokazooie/texturepacks/the_nameless/thenameless-banjo-kazooie-texture-coverpic_hu_3756fbe5b286b70e.jpg" },
    { "Banjo-Kazooie", "https://files.emulationking.com/n64/texturepacks/banjokazooie/banjo-kazooie_v-king.zip", "", "Retexture", "V-King", NULL, 5549062u, "https://emulationking.com/nintendo/n64/games/banjokazooie/texturepacks/v-king/v-king-banjo-kazooie-texture-coverpic_hu_101a86fbd186b6ad.jpg" },
    { "CASTLEVANIA", "https://files.emulationking.com/n64/texturepacks/castlevania64/castlevania64-googol.zip", "", "Castlevania HD", "Googol", NULL, 1303662u, "https://emulationking.com/nintendo/n64/games/castlevania64/texturepacks/googol/googols-castlevania64-texture-pack-coverpic_hu_57c461e6665fba4b.jpg" },
    { "CASTLEVANIA2", "https://files.emulationking.com/n64/texturepacks/castlevania-legacyofdarkness/CastleVania-Legacy_of_Darkness-gitechs.zip", "", "Legacy of Darkness HD", "gitech", NULL, 443019421u, "https://emulationking.com/nintendo/n64/games/castlevanialegacydarkness/texturepacks/gitech/gitechs-castlevania-legacy-of-darkness-texture-pack-coverpic_hu_fb52877703f48da7.jpg" },
    { "CONKER BFD", "https://files.emulationking.com/n64/texturepacks/cbfd/ConkerBFD-Kazangwa.zip", "", "Conker HD", "Kazangwa", NULL, 2164402u, "https://emulationking.com/nintendo/n64/games/conkersbadfurday/texturepacks/kazangwa/kazangwass-conkers-bad-fur-day-texture-pack-pic-coverpic_hu_442c770e8dea0800.jpg" },
    { "Diddy Kong Racing", "https://files.emulationking.com/n64/texturepacks/diddykongracing/diddy_kong_racing_pm_.zip", "", "Diddy Kong Racing HD", "PM", NULL, 7534193u, "https://emulationking.com/nintendo/n64/games/diddykongracing/texturepacks/pm/pm_-Diddy-Kong-Racing-Texture-Pack-Cover-Pic_hu_39a65757a348e2aa.jpg" },
    { "DUKE NUKEM", "https://files.emulationking.com/n64/texturepacks/dukenukem64/Duke-Nukem-64-somethingevil.zip", "", "Duke Nukem 64 HD", "SomeThingEviL", NULL, 135965035u, "https://emulationking.com/nintendo/n64/games/dukenukem64/texturepacks/somethingevil/SomeThingEvils-Duke-Nukem-64-Texture-Pack-Cover_hu_b2b623360625789d.jpg" },
    { "HARVESTMOON64", "https://files.emulationking.com/n64/texturepacks/harvestmoon64/harvestmoon64-coffeeandtv.zip", "", "Harvest Moon 64 HD", "CoffeeAndTV", NULL, 875478u, "https://emulationking.com/nintendo/n64/games/harvestmoon64/texturepacks/coffeeandtv/coffeeandtv-Harvest-Moon-64-Texture-Pack-Cover-Pic_hu_7fc5bc9be5af8a2a.jpg" },
    { "MARIOKART64", "https://files.emulationking.com/n64/texturepacks/mariokart64/Mario_Kart_64-BFrancois.zip", "", "Super Mario Kart 64 HD", "BFrancois", NULL, 120072291u, "https://emulationking.com/nintendo/n64/games/mariokart64/texturepacks/bfrancois/bfrancois-Mario-Kart-64-Texture-Pack-Cover-PIC_hu_84dc9d98b48aed99.jpg" },
    { "MARIOKART64", "https://files.emulationking.com/n64/texturepacks/mariokart64/Mario_kart_64-Kerber2k.zip", "", "Mario Kart 64 HD", "Kerber2k", NULL, 18161452u, "https://emulationking.com/nintendo/n64/games/mariokart64/texturepacks/kerberk2k/Kerber2k-Mario-kart-64-Texture-Pack-Cover-Pic_hu_846441136ee44135.jpg" },
    { "MARIOKART64", "https://files.emulationking.com/n64/texturepacks/mariokart64/Mario_Kart_64-ambar_PDF_Lepascals.zip", "", "Retexture", "Ambar, PDK, Lepascals", NULL, 848908u, "https://emulationking.com/nintendo/n64/games/mariokart64/texturepacks/ambarpdklepascals/ambar-pdk-lepascals-Mario-Kart-64-Texture-Pack-Cover-Pic_hu_9ab5fdd4a9e523a9.jpg" },
    { "MARIOKART64", "https://files.emulationking.com/n64/texturepacks/mariokart64/Mario_Kart_64-Leonardo20.zip", "", "Retexture", "Leonardo20", NULL, 864401u, "https://emulationking.com/nintendo/n64/games/mariokart64/texturepacks/leonardo20/Leonardo20-Mario-Kart-64-Texture-Pack-Cover-Pic_hu_ecee1f406fb7946e.jpg" },
    { "MARIOKART64", "https://files.emulationking.com/n64/texturepacks/mariokart64/Mario_Kart_64-RiSiO_Raceway.zip", "", "Raceway", "RiSiO", NULL, 444655u, "https://emulationking.com/nintendo/n64/games/supermario64/texturepacks/risio/Risio-Retro-Super-Mario-64-Texture-Pack-Cover-Pic_hu_42bead11ed731b93.jpg" },
    { "MARIOKART64", "https://files.emulationking.com/n64/texturepacks/mariokart64/Mario_Kart_64-Koopa_Troopa.zip", "", "Koopa Troopa", "Skielled Slackers", NULL, 862414u, "https://emulationking.com/nintendo/n64/games/mariokart64/texturepacks/skielledslackers/Skielledslacker-kooper-back-Mario-Kart-64-Texture-Pack-Cover-Pic_hu_7002540be8172dd0.jpg" },
    { "PAPER MARIO", "https://files.emulationking.com/n64/texturepacks/papermario/paper-mario-mixed-authors.zip", "", "Paper Mario HD", "Mollymutt and others", NULL, 49751698u, "https://emulationking.com/nintendo/n64/games/supermario64/texturepacks/mollymutt/Mollymutt-Super-Mario-64-Texture-Pack-Cover-Pic_hu_2b775c2112543d6e.jpg" },
    { "Shadowman", "https://files.emulationking.com/n64/texturepacks/shadowman/Shadowman-MasterV.zip", "", "Shadowman HD", "MasterV", NULL, 215431839u, "https://emulationking.com/nintendo/n64/games/shadowman/texturepacks/masterv/MasterV-Shadow-Man-Texture-Pack-Cover-Pic_hu_2533ad2b44d55887.jpg" },
    { "TSUMI TO BATSU", "https://files.emulationking.com/n64/texturepacks/sinandpunishment/Sin_and_Punishment_english-vanit.zip", "", "Sin and Punishment HD", "Vanit", NULL, 4713079u, "https://emulationking.com/nintendo/n64/games/sinandpunishment/texturepacks/vanit/Vanits-SIn-and-Punishment-English-Translation-Cover-Pic_hu_20f714b7d22de671.jpg" },
    { "STARFOX64", "https://files.emulationking.com/n64/texturepacks/starfox64/STARFOX64-Razius.zip", "", "Star Fox 64 HD", "Razius", NULL, 142700796u, "https://emulationking.com/nintendo/n64/games/starfox64/texturepacks/razius/Razius-Star-Fox-64-Texture-Pack-Cover-Pic_hu_6fa389536278a60d.jpg" },
    { "STARFOX64", "https://files.emulationking.com/n64/texturepacks/starfox64/StarFox64-Unaidedcoder.zip", "", "Star Fox 64 HD", "UnaidedCoder", NULL, 25792560u, "https://emulationking.com/nintendo/n64/games/starfox64/texturepacks/unaidedcoder/UnaidedCoders-Star-Fox-64-Texture-Pack-Cover-Pic_hu_aefa6a27a6b92110.jpg" },
    { "SUPER MARIO 64", "https://files.emulationking.com/n64/texturepacks/supermario64/super_mario_64-Razius.zip", "", "Super Mario 64 HD", "MU-TH-UR, Razius", NULL, 624776331u, "https://emulationking.com/nintendo/n64/games/supermario64/texturepacks/mu-th-ur/muthorrazius-Super-Mario-64-texture-pack-cover-pic_hu_5ea754c5b1bf3824.jpg" },
    { "SUPER MARIO 64", "https://files.emulationking.com/n64/texturepacks/supermario64/super_mario_64-mollymutt.zip", "", "Super Mario 64 HD", "Mollymutt", NULL, 70326577u, "https://emulationking.com/nintendo/n64/games/supermario64/texturepacks/mollymutt/Mollymutt-Super-Mario-64-Texture-Pack-Cover-Pic_hu_2b775c2112543d6e.jpg" },
    { "SUPER MARIO 64", "https://files.emulationking.com/n64/texturepacks/supermario64/super_mario_64-Nintemod.zip", "", "Super Mario 64 HD", "Nintemods", NULL, 15691918u, "https://emulationking.com/nintendo/n64/games/supermario64/texturepacks/nintemods/Nintemod-Super-Mario-64-Texture-Pack-Cover-Pic_hu_eb7e466e6394b7e7.jpg" },
    { "SUPER MARIO 64", "https://files.emulationking.com/n64/texturepacks/supermario64/super_mario_64-mode7.zip", "", "Retexture", "Mode7", NULL, 11562349u, "https://emulationking.com/nintendo/n64/games/supermario64/texturepacks/mode7/mode7s-Super-Mario-64-Texture-Pack-Cover-Pic_hu_a558378da8337c28.jpg" },
    { "SUPER MARIO 64", "https://files.emulationking.com/n64/texturepacks/supermario64/super_mario_64-Hizoka10.zip", "", "Retexture", "Hizoka10", NULL, 14107622u, "https://emulationking.com/nintendo/n64/games/supermario64/texturepacks/hizoka10/Hizoka10s-Super-Mario-64-Texture-Pack-Cover-Pic_hu_57ce165824eb4e4a.jpg" },
    { "SUPER MARIO 64", "https://files.emulationking.com/n64/texturepacks/supermario64/super_mario_64-Risios.zip", "", "Retro", "RiSiO", NULL, 8162855u, "https://emulationking.com/nintendo/n64/games/supermario64/texturepacks/risio/Risio-Retro-Super-Mario-64-Texture-Pack-Cover-Pic_hu_42bead11ed731b93.jpg" },
    { "SUPER MARIO 64", "https://files.emulationking.com/n64/texturepacks/supermario64/super_mario_64-Kamran.zip", "", "Retexture", "Kamran10", NULL, 517815u, "https://emulationking.com/nintendo/n64/games/supermario64/texturepacks/kamran10/Kamran-Super-Mario-64-Texture-Pack-Pic-01_hu_8f2e084fab7c4b8c.jpg" },
    { "SUPER MARIO 64", "https://files.emulationking.com/n64/texturepacks/supermario64/super_mario_64-LexLuthor.zip", "", "Retexture", "LexLuthor", NULL, 171272u, "https://emulationking.com/nintendo/n64/games/supermario64/texturepacks/lexluthor/LexLuthor-Super-Mario-64-Paint-Texture-Pack-Cover-Pic_hu_d41aee4f9cbf05bd.jpg" },
    { "SMASH BROTHERS", "https://files.emulationking.com/n64/texturepacks/supersmashbros/Super_Smash_Bros-Pietschie_and_Bad_Randolph.zip", "", "Smash Bros. HD", "Pietschie, Bad Randolph", NULL, 55119143u, "https://emulationking.com/nintendo/n64/games/supersmashbros/texturepacks/pietschiebadrandolph/Pietschie-and-bad-randolph-Super-Smash-Bros-Texture-Pack-Cover-Pic_hu_ffb8d76affd708a9.jpg" },
    { "ZELDA MAJORA'S MASK", "https://files.emulationking.com/n64/texturepacks/majorasmask/Majoras-Mask-Bluedart.zip", "", "Majora's Mask HD", "Bluedart", NULL, 428483539u, "https://emulationking.com/nintendo/n64/games/zeldamajorasmask/texturepacks/bluedart/Bluedarts-Majoras-Mask-Texture-Pack-Cover-Pic_hu_7872d2a82b9ee75e.jpg" },
    { "ZELDA MAJORA'S MASK", "https://files.emulationking.com/n64/texturepacks/majorasmask/Djipis-Majoras-Mask-3DS-2016-Rice.zip", "", "3DS Style 2016", "Djipi", NULL, 49540014u, "https://emulationking.com/nintendo/n64/games/zeldaocarinaoftime/texturepacks/djipi20163ds/djipis-ocarina-of-time-3ds-texture-pack-2016-cover_hu_eaaa433008888d51.jpg" },
    { "ZELDA MAJORA'S MASK", "https://files.emulationking.com/n64/texturepacks/majorasmask/Djipis-Majoras-Mask-Celda-2016-Rice.zip", "", "Celda 2016", "Djipi", NULL, 118594081u, "https://emulationking.com/nintendo/n64/games/zeldamajorasmask/texturepacks/djipi2016celda/Djipi-2016-Celda-Majoras-Mask-Texture-Pack-Cover-Pic_hu_5575dd33d97b6656.jpg" },
    { "ZELDA MAJORA'S MASK", "https://files.emulationking.com/n64/texturepacks/majorasmask/Djipis-Majoras-Mask-Celda-2011-Rice.zip", "", "Celda 2011", "Djipi", NULL, 109628558u, "https://emulationking.com/nintendo/n64/games/zeldamajorasmask/texturepacks/djipi2011celda/Djipis-2011-Majoras-Mask-Texture-Pack-Cover-Pic_hu_8891fbd63beff5db.jpg" },
    { "ZELDA MAJORA'S MASK", "https://files.emulationking.com/n64/texturepacks/majorasmask/Djipis-Majoras-Mask-Celda-2009-Rice.zip", "", "Cel Shade V1", "Djipi, mdtauk", NULL, 55642626u, "https://emulationking.com/nintendo/n64/games/zeldamajorasmask/texturepacks/djipicellv1/Djipis-Cel-Shade-v1-Majoras-Mask-Texture-Pack-Cover-Pic_hu_7efc4e3f8e4e85a7.jpg" },
    { "ZELDA MAJORA'S MASK", "https://files.emulationking.com/n64/texturepacks/majorasmask/Djipis-Majoras-Mask-3ds-2009-Rice.zip", "", "Realistic 2009", "Djipi", NULL, 22412229u, "https://emulationking.com/nintendo/n64/games/zeldamajorasmask/texturepacks/djipirealistic/Djipi-Realistic-Majoras-Mask-Texture-Pack-Cover-Pic_hu_db2e51f0423c8b1d.jpg" },
    { "THE LEGEND OF ZELDA", "https://files.emulationking.com/n64/texturepacks/oot/OOT-Community_Edition.zip", "", "Community Pack", "EmuTalk community", NULL, 838795511u, "https://emulationking.com/nintendo/n64/games/zeldaocarinaoftime/texturepacks/community/Community-Ocarina-of-Time-Texture-Pack-Cover-Pic_hu_4c6f348d23549b24.jpg" },
    { "THE LEGEND OF ZELDA", "https://files.emulationking.com/n64/texturepacks/oot/Djipi_Zelda_HD_2015_Rice.zip", "", "NEW 2014", "Djipi", NULL, 89035028u, "https://emulationking.com/nintendo/n64/games/zeldaocarinaoftime/texturepacks/djipi2014new/Djipis-2014-New-Style-Ocarina-of-Time-Texture-Pack-Cover-Pic_hu_6946405a4ca70eee.jpg" },
    { "THE LEGEND OF ZELDA", "https://files.emulationking.com/n64/texturepacks/oot/Djipi_Zelda_3DS_2016_Rice.zip", "", "3DS Style 2016", "Djipi", NULL, 89012170u, "https://emulationking.com/nintendo/n64/games/zeldaocarinaoftime/texturepacks/djipi20163ds/djipis-ocarina-of-time-3ds-texture-pack-2016-cover_hu_eaaa433008888d51.jpg" },
    { "THE LEGEND OF ZELDA", "https://files.emulationking.com/n64/texturepacks/oot/Djipi_Zelda_Cellshade_2016_Rice.zip", "", "Cel Shade 2016", "Djipi", NULL, 108562749u, "https://emulationking.com/nintendo/n64/games/zeldaocarinaoftime/texturepacks/djipi2016cellshade/Djipis-2016-Celda-ocarina-of-time-texture-pack-Cover-Pic_hu_3f1f5020e7b95adf.jpg" },
    { "THE LEGEND OF ZELDA", "https://files.emulationking.com/n64/texturepacks/oot/Djipi_Zelda_Cellshade_2011_Rice.zip", "", "Cel Shade 2011", "Djipi", NULL, 107225675u, "https://emulationking.com/nintendo/n64/games/zeldaocarinaoftime/texturepacks/djipi2011cellshade/Djipis-Cell-Shade-ocarina-of-time-texture-pack-Cover-Pic_hu_c48966714e2a4c8b.jpg" },
    { "THE LEGEND OF ZELDA", "https://files.emulationking.com/n64/texturepacks/oot/Djipi_Zelda_Cellshade_2009.zip", "", "Cel Shade 2009", "Djipi", NULL, 123753867u, "https://emulationking.com/nintendo/n64/games/zeldaocarinaoftime/texturepacks/djipi2009cellshade/OOT-Djipi-2009-Cover-Pic_hu_64c70c05c2e7a5fd.jpg" },
    { "THE LEGEND OF ZELDA", "https://files.emulationking.com/n64/texturepacks/oot/OOT-Phantom7.zip", "", "Ocarina of Time HD", "Phantom7", NULL, 179737130u, "https://emulationking.com/nintendo/n64/games/zeldaocarinaoftime/texturepacks/phantom7/Phantom7s-Ocarina-of-Time-Retexture-COver-Pic_hu_695caf51501eaf9e.jpg" },
    { "THE LEGEND OF ZELDA", "https://files.emulationking.com/n64/texturepacks/oot/OOT-Zeth_Alkar.zip", "", "Ocarina of Time HD", "Zeth Alkar", NULL, 115840872u, "https://emulationking.com/nintendo/n64/games/zeldaocarinaoftime/texturepacks/zethalkar/Zeth-Alkars-OOT-Texture-Pack-Cover-Pic_hu_b137361bf16545fb.jpg" },
    { "THE LEGEND OF ZELDA", "https://files.emulationking.com/n64/texturepacks/oot/OOT-Federelli.zip", "", "Ocarina of Time HD", "Federelli", NULL, 55841352u, "https://emulationking.com/nintendo/n64/games/zeldaocarinaoftime/texturepacks/federelli/Ocarina-Of-Time-Federelli-Cover-Pic_hu_197bf16aa040eb82.jpg" },
    { "AEROGAUGE", "https://www.n64textures.com/download/AEROGAUGE_v1_By_N64TEX_(_.zip", "", "AeroGauge HD", "N64TEX", NULL, 113506064u, NULL },
    { "ZELDA MAJORA'S MASK", "MM-Reloaded", "GhostlyDark", "MM Reloaded", "GhostlyDark", NULL, 0, "https://raw.githubusercontent.com/GhostlyDark/MM-Reloaded/HEAD/mm-reloaded.jpg" },
};
#define PACK_COUNT ((int)(sizeof(s_packs) / sizeof(s_packs[0])))

void HiresFolderName(const char *romName, char *out, size_t size)
{
    size_t i;
    if (!size) return;
    for (i = 0; romName && romName[i] && i + 1 < size; i++)
        out[i] = strchr(":*?\"<>|/\\", romName[i]) ? '-' : romName[i];
    out[i] = 0;
}

int HiresPackCountFor(const char *romName)
{
    char name[64];
    int i, n = 0;
    HiresFolderName(romName, name, sizeof(name));
    for (i = 0; i < PACK_COUNT; i++)
        if (!strcmp(s_packs[i].romName, name)) n++;
    return n;
}

const HiresPackEntry *HiresPackGet(const char *romName, int k)
{
    char name[64];
    int i;
    HiresFolderName(romName, name, sizeof(name));
    for (i = 0; i < PACK_COUNT; i++)
        if (!strcmp(s_packs[i].romName, name) && k-- == 0) return &s_packs[i];
    return NULL;
}

// ---- The worker's state ----
static volatile LONG s_state, s_error, s_cancel, s_doneKb, s_totalKb;
static char s_file[128];                 // the .hts installed (HPACK_DONE)
static HiresPackEntry s_job;
static char s_dest[192];
static NetProgress s_dlNet;                // the download's bytes (HPACK_DOWNLOAD)

static int fail(int error)
{
    InterlockedExchange(&s_error, error);
    InterlockedExchange(&s_state, HPACK_FAILED);
    return 0;
}

// ---- The release (GitHub API) ----
// The .hts pack of the latest release: its download URL and size. The HD one
// when there are HD and 4K (the 4K ones come in parts too large for the
// console), else the one .hts; never a split archive (.7z.001).
static int find_asset(const char *json, const char *want, char *url, size_t urlSize, unsigned *size)
{
    static const char *const kinds[] = { "hts-hd.7z", "hts.7z" };
    const char *key = "\"browser_download_url\":\"";
    int k;
    for (k = 0; k < (want ? 1 : 2); k++)
    {
        const char *p = json, *prev = json;
        while ((p = strstr(p, key)) != NULL)
        {
            const char *u = p + strlen(key), *e = strchr(u, '"');
            const char *kind = want ? want : kinds[k];   // a pack's own (a .zip of PNGs), else the .hts ones
            size_t n, kl = strlen(kind);
            if (!e) break;
            n = (size_t)(e - u);
            if (n > kl && n < urlSize && !memcmp(e - kl, kind, kl))
            {
                // Its size: the asset's "size" comes before its URL, after the previous asset's.
                const char *s = NULL, *q = prev;
                while ((q = strstr(q, "\"size\":")) != NULL && q < p) { s = q; q++; }
                memcpy(url, u, n);
                url[n] = 0;
                *size = s ? (unsigned)strtoul(s + 7, NULL, 10) : 0;
                return 1;
            }
            prev = e;
            p = e;
        }
    }
    return 0;
}

// ---- Free space ----
static int space_for(const char *dir, unsigned __int64 bytes)
{
    char root[16];
    const char *colon = strchr(dir, ':');
    ULARGE_INTEGER avail, total, freeAll;
    size_t n = colon ? (size_t)(colon - dir) + 1 : 0;
    if (!n || n + 2 > sizeof(root)) return 1;   // unknown: let the write tell
    memcpy(root, dir, n);
    root[n] = '\\';
    root[n + 1] = 0;
    if (!GetDiskFreeSpaceExA(root, &avail, &total, &freeAll)) return 1;
    return avail.QuadPart >= bytes + (8u << 20);   // and a little room
}

// ---- The .7z: its index read by the LZMA SDK, the .hts unpacked here ----
static void *sz_alloc(ISzAllocPtr, size_t size) { return size ? malloc(size) : NULL; }
static void sz_free(ISzAllocPtr, void *address) { free(address); }
static const ISzAlloc s_alloc = { sz_alloc, sz_free };

struct FileStream { ISeekInStream vt; FILE *f; };
static SRes fs_read(const ISeekInStream *p, void *buf, size_t *size)
{
    FileStream *s = (FileStream *)p;
    *size = fread(buf, 1, *size, s->f);
    return ferror(s->f) ? SZ_ERROR_READ : SZ_OK;
}
static SRes fs_seek(const ISeekInStream *p, Int64 *pos, ESzSeek origin)
{
    FileStream *s = (FileStream *)p;
    int whence = origin == SZ_SEEK_SET ? SEEK_SET : origin == SZ_SEEK_CUR ? SEEK_CUR : SEEK_END;
    if (*pos > 0x7FFFFFFF || *pos < -0x7FFFFFFF || fseek(s->f, (long)*pos, whence)) return SZ_ERROR_READ;   // archives under 2 GB
    *pos = ftell(s->f);
    return SZ_OK;
}

// ---- The .hts made smaller: its RGBA8 textures to DXT ----
// The packs keep their textures as raw RGBA8 (4 bytes a texel; Banjo-Tooie's
// 18 MB archive unpacks to 138 MB). They are rewritten as they come out of the
// archive: DXT1 for the opaque ones, DXT5 for the others (8 and 4 times
// smaller), which the reader decodes (render/xenos/h64_hts.cpp). The records
// move, so the index at the end gets their new offsets. Less to write to the
// USB drive is also what makes the unpacking faster.
static u32 rd32le(const u8 *p) { return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24; }
static u64 rd64le(const u8 *p) { return rd32le(p) | (u64)rd32le(p + 4) << 32; }
static void wr16le(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static void wr32le(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24); }
static void wr64le(u8 *p, u64 v) { wr32le(p, (u32)v); wr32le(p + 4, (u32)(v >> 32)); }

enum { HTS_RGBA8 = 0x8058, HTS_RGBA = 0x1908, HTS_UBYTE = 0x1401, HTS_DXT1 = 0x83F1, HTS_DXT5 = 0x83F3 };

static u32 e5(u32 v) { return (v << 3) | (v >> 2); }

// The 4 colours a block's endpoints give, as the reader works them out.
static void dxt_palette(u32 c0, u32 c1, int pal[4][3])
{
    int k;
    pal[0][0] = e5(c0 >> 11 & 31); pal[0][1] = (c0 >> 5 & 63) << 2 | (c0 >> 9 & 3); pal[0][2] = e5(c0 & 31);
    pal[1][0] = e5(c1 >> 11 & 31); pal[1][1] = (c1 >> 5 & 63) << 2 | (c1 >> 9 & 3); pal[1][2] = e5(c1 & 31);
    for (k = 0; k < 3; k++)
    {
        pal[2][k] = (2 * pal[0][k] + pal[1][k]) / 3;
        pal[3][k] = (pal[0][k] + 2 * pal[1][k]) / 3;
    }
}

static u32 to565(float r, float g, float b)
{
    int ri = (int)(r * 31.0f / 255.0f + 0.5f), gi = (int)(g * 63.0f / 255.0f + 0.5f), bi = (int)(b * 31.0f / 255.0f + 0.5f);
    ri = ri < 0 ? 0 : ri > 31 ? 31 : ri;
    gi = gi < 0 ? 0 : gi > 63 ? 63 : gi;
    bi = bi < 0 ? 0 : bi > 31 ? 31 : bi;
    return (u32)(ri << 11 | gi << 5 | bi);
}

// A 4x4 block's colours (px: 16 RGBA texels) to its 8 bytes: the endpoints on
// the colours' main axis, pulled in a little, each texel the nearest of 4.
static void dxt_color(const u8 *px, u8 *out)
{
    float mean[3] = { 0, 0, 0 }, cov[6] = { 0, 0, 0, 0, 0, 0 }, axis[3], lo = 1e9f, hi = -1e9f, e0[3], e1[3];
    int pal[4][3], i, k, it;
    u32 c0, c1, bits = 0;
    for (i = 0; i < 16; i++)
        for (k = 0; k < 3; k++) mean[k] += px[i * 4 + k];
    for (k = 0; k < 3; k++) mean[k] *= 1.0f / 16.0f;
    for (i = 0; i < 16; i++)
    {
        float r = px[i * 4] - mean[0], g = px[i * 4 + 1] - mean[1], b = px[i * 4 + 2] - mean[2];
        cov[0] += r * r; cov[1] += r * g; cov[2] += r * b; cov[3] += g * g; cov[4] += g * b; cov[5] += b * b;
    }
    // Its main axis (a few power iterations, from the largest spread).
    axis[0] = cov[0] + 1.0f; axis[1] = cov[3] + 1.0f; axis[2] = cov[5] + 1.0f;
    for (it = 0; it < 4; it++)
    {
        float x = cov[0] * axis[0] + cov[1] * axis[1] + cov[2] * axis[2];
        float y = cov[1] * axis[0] + cov[3] * axis[1] + cov[4] * axis[2];
        float z = cov[2] * axis[0] + cov[4] * axis[1] + cov[5] * axis[2];
        float m = x * x + y * y + z * z;
        if (m < 1e-6f) { x = y = z = 0.57735f; m = 1.0f; }
        m = 1.0f / sqrtf(m);
        axis[0] = x * m; axis[1] = y * m; axis[2] = z * m;
    }
    for (i = 0; i < 16; i++)
    {
        float t = (px[i * 4] - mean[0]) * axis[0] + (px[i * 4 + 1] - mean[1]) * axis[1] + (px[i * 4 + 2] - mean[2]) * axis[2];
        if (t < lo) lo = t;
        if (t > hi) hi = t;
    }
    // Pulled in by 1/16 of the range: the ends are rarely hit exactly.
    {
        float in = (hi - lo) / 16.0f;
        lo += in; hi -= in;
    }
    for (k = 0; k < 3; k++) { e0[k] = mean[k] + axis[k] * hi; e1[k] = mean[k] + axis[k] * lo; }
    c0 = to565(e0[0], e0[1], e0[2]);
    c1 = to565(e1[0], e1[1], e1[2]);
    if (c0 < c1) { u32 t = c0; c0 = c1; c1 = t; }   // c0 > c1: the 4-colour mode
    if (c0 != c1)
    {
        dxt_palette(c0, c1, pal);
        for (i = 0; i < 16; i++)
        {
            int best = 0, bestErr = 0x7FFFFFFF;
            for (k = 0; k < 4; k++)
            {
                int dr = px[i * 4] - pal[k][0], dg = px[i * 4 + 1] - pal[k][1], db = px[i * 4 + 2] - pal[k][2];
                int e = dr * dr + dg * dg + db * db;
                if (e < bestErr) { bestErr = e; best = k; }
            }
            bits |= (u32)best << (i * 2);
        }
    }
    // (equal endpoints: one colour, every texel index 0)
    wr16le(out, c0);
    wr16le(out + 2, c1);
    wr32le(out + 4, bits);
}

// A block's alpha (DXT5's 8 bytes): its lowest and highest, the 8-step mode.
static void dxt_alpha(const u8 *px, u8 *out)
{
    u32 lo = 255, hi = 0, al[8], i, k;
    u64 bits = 0;
    for (i = 0; i < 16; i++)
    {
        u32 a = px[i * 4 + 3];
        if (a < lo) lo = a;
        if (a > hi) hi = a;
    }
    out[0] = (u8)hi;
    out[1] = (u8)lo;
    if (hi > lo)
    {
        al[0] = hi; al[1] = lo;
        for (k = 1; k < 7; k++) al[k + 1] = ((7 - k) * hi + k * lo) / 7;
        for (i = 0; i < 16; i++)
        {
            u32 a = px[i * 4 + 3], best = 0, bestErr = 256;
            for (k = 0; k < 8; k++)
            {
                u32 e = a > al[k] ? a - al[k] : al[k] - a;
                if (e < bestErr) { bestErr = e; best = k; }
            }
            bits |= (u64)best << (3 * i);
        }
    }
    for (i = 0; i < 6; i++) out[2 + i] = (u8)(bits >> (8 * i));
}

// A w x h RGBA8 texture to DXT1 (alpha: 0) or DXT5; the edge blocks repeat the edge texels.
static void dxt_encode(const u8 *src, u32 w, u32 h, int alpha, u8 *dst)
{
    u32 bx, by, i;
    u8 px[64];
    for (by = 0; by < (h + 3) / 4; by++)
        for (bx = 0; bx < (w + 3) / 4; bx++)
        {
            for (i = 0; i < 16; i++)
            {
                u32 x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
                if (x >= w) x = w - 1;
                if (y >= h) y = h - 1;
                memcpy(px + i * 4, src + (y * w + x) * 4, 4);
            }
            if (alpha) { dxt_alpha(px, dst); dxt_color(px, dst + 8); dst += 16; }
            else { dxt_color(px, dst); dst += 8; }
        }
}

// The .hts as it comes out of the archive: its header (16 bytes), its records
// (23-byte header, data), up to indexPos, then the index (to its end).
enum { SH_HEADER, SH_RECORD_HEAD, SH_RECORD, SH_INDEX };
struct Shrink
{
    FILE *out;
    int state, err;
    size_t need;                          // bytes `buf` waits for
    u64 indexPos, recStart, written;
    std::vector<u8> buf, index, dxt, raw;   // raw: a zlib record's pixels
    std::vector<u64> oldOff, newOff;      // where each record was, where it is now
    u32 converted;
};

static void shrink_next(Shrink *s)
{
    s->buf.clear();
    if (s->recStart == s->indexPos) s->state = SH_INDEX;
    else { s->state = SH_RECORD_HEAD; s->need = 23; }
}

static int shrink_write(Shrink *s, const void *p, size_t n)
{
    if (fwrite(p, 1, n, s->out) != n) { s->err = HPERR_SPACE; return 0; }
    s->written += n;
    return 1;
}

// A whole record (s->buf): written, as DXT when it is an RGBA8 one.
static voidpf sh_zalloc(voidpf, uInt items, uInt size) { return malloc((size_t)items * size); }
static void sh_zfree(voidpf, voidpf p) { free(p); }

static int shrink_record(Shrink *s)
{
    u8 *rec = &s->buf[0];
    u32 w = rd32le(rec), h = rd32le(rec + 4), format = rd32le(rec + 8), dataSize = rd32le(rec + 19);
    u32 texFmt = rec[12] | rec[13] << 8, pixType = rec[14] | rec[15] << 8;
    const u8 *data = rec + 23;
    s->oldOff.push_back(s->recStart);
    s->newOff.push_back(s->written);
    if (format == (0x80000000u | HTS_RGBA8) && texFmt == HTS_RGBA && pixType == HTS_UBYTE && w && h && w <= 4096 && h <= 4096)
    {
        // zlib-packed RGBA8 (Smash Remix Ultimate's pack): unpacked here and made DXT as the
        // others, so the game neither inflates it nor uploads it whole (kept as it is if it fails).
        z_stream z;
        int r;
        s->raw.resize(w * h * 4);
        memset(&z, 0, sizeof(z));
        z.zalloc = sh_zalloc;
        z.zfree = sh_zfree;
        if (inflateInit(&z) == Z_OK)
        {
            z.next_in = (Bytef *)data; z.avail_in = dataSize;
            z.next_out = &s->raw[0]; z.avail_out = w * h * 4;
            r = inflate(&z, Z_FINISH);
            if (r == Z_STREAM_END && z.total_out == w * h * 4) { data = &s->raw[0]; dataSize = w * h * 4; format = HTS_RGBA8; }
            inflateEnd(&z);
        }
    }
    if (format == HTS_RGBA8 && texFmt == HTS_RGBA && pixType == HTS_UBYTE && w && h && w <= 4096 && h <= 4096 && dataSize == w * h * 4)
    {
        u32 i, alpha = 0, size;
        for (i = 3; i < dataSize && !alpha; i += 4) alpha = data[i] != 255;
        size = ((w + 3) / 4) * ((h + 3) / 4) * (alpha ? 16 : 8);
        s->dxt.resize(size);
        dxt_encode(data, w, h, alpha, &s->dxt[0]);
        wr32le(rec + 8, alpha ? HTS_DXT5 : HTS_DXT1);
        wr32le(rec + 19, size);
        s->converted++;
        return shrink_write(s, rec, 23) && shrink_write(s, &s->dxt[0], size);
    }
    return shrink_write(s, rec, s->buf.size());
}

static int shrink_feed(Shrink *s, const u8 *p, size_t n)
{
    while (n)
    {
        size_t take;
        if (s->state == SH_INDEX)
        {
            if (s->index.size() + n > (64u << 20)) return 0;
            s->index.insert(s->index.end(), p, p + n);
            return 1;
        }
        take = s->need - s->buf.size();
        if (take > n) take = n;
        s->buf.insert(s->buf.end(), p, p + take);
        p += take;
        n -= take;
        if (s->buf.size() < s->need) continue;
        if (s->state == SH_HEADER)
        {
            s->indexPos = rd64le(&s->buf[8]);
            if (s->indexPos < 16 || s->indexPos > 0x7FFFFFFFu) return 0;
            if (!shrink_write(s, &s->buf[0], 16)) return 0;   // its indexPos set at the end
            s->recStart = 16;
            shrink_next(s);
        }
        else if (s->state == SH_RECORD_HEAD)
        {
            u64 size = 23 + (u64)rd32le(&s->buf[19]);
            if (s->recStart + size > s->indexPos || size > (80u << 20)) return 0;
            s->need = (size_t)size;
            s->buf.reserve(s->need);
            s->state = SH_RECORD;
        }
        else
        {
            if (!shrink_record(s)) return 0;
            s->recStart += s->need;
            shrink_next(s);
        }
    }
    return 1;
}

// The index, with the records' new offsets, after them; the header pointing to it.
static int shrink_finish(Shrink *s)
{
    u64 bytes = s->index.size(), n, i, kept = 0;
    std::vector<u8> idx;
    u8 pos[8];
    u32 count;
    int withCount;
    if (s->state != SH_INDEX || bytes < 16) return 0;
    count = rd32le(&s->index[0]);
    // With or without its count, as the reader takes it.
    withCount = (bytes - 4) % 16 == 0 && (u64)count * 16 == bytes - 4;
    n = withCount ? count : bytes / 16;
    idx.resize(4 + (size_t)n * 16);
    for (i = 0; i < n; i++)
    {
        const u8 *e = &s->index[(withCount ? 4 : 0) + (size_t)i * 16];
        // The offset is the low 48 bits; GLideN64 keeps flags above them (kept as they are).
        u64 raw = rd64le(e + 8), off = raw & 0x0000FFFFFFFFFFFFull;
        std::vector<u64>::const_iterator it = std::lower_bound(s->oldOff.begin(), s->oldOff.end(), off);
        if (it == s->oldOff.end() || *it != off) continue;   // not a record's start: the reader skips it too
        memcpy(&idx[4 + (size_t)kept * 16], e, 8);
        wr64le(&idx[4 + (size_t)kept * 16 + 8], (raw & ~0x0000FFFFFFFFFFFFull) | s->newOff[it - s->oldOff.begin()]);
        kept++;
    }
    wr32le(&idx[0], (u32)kept);
    wr64le(pos, s->written);
    if (!shrink_write(s, &idx[0], 4 + (size_t)kept * 16)) return 0;
    if (fseek(s->out, 8, SEEK_SET) || fwrite(pos, 1, 8, s->out) != 8) { s->err = HPERR_SPACE; return 0; }
    return 1;
}

#define UNPACK_IN (256u << 10)
#define UNPACK_OUT (1u << 20)
#define UNPACK_WRITE (1u << 20)   // the .hts written in big pieces (USB drives)

// The archive's .hts unpacked into dir\<its name>; its name to `name`.
static int unpack(const char *archive, const char *dir, char *name, size_t nameSize)
{
    FileStream fs;
    CLookToRead2 look;
    CSzArEx db;
    FILE *in, *out = NULL;
    UInt16 *wname = NULL;
    Byte *inBuf = NULL, *outBuf = NULL;
    CLzmaDec lz;
    CLzma2Dec lz2;
    Shrink sh;
    int ok = 0, err = HPERR_ARCHIVE, lzma2 = 0, opened = 0, decoder = 0;
    UInt32 i, file = 0xFFFFFFFFu, folder;
    char path[256], part[256];
    in = fopen(archive, "rb");
    if (!in) return fail(HPERR_FILE);
    fs.vt.Read = fs_read;
    fs.vt.Seek = fs_seek;
    fs.f = in;
    LookToRead2_CreateVTable(&look, False);
    look.buf = (Byte *)malloc(1u << 16);
    look.bufSize = 1u << 16;
    look.realStream = &fs.vt;
    LookToRead2_Init(&look);
    CrcGenerateTable();
    SzArEx_Init(&db);
    if (!look.buf || SzArEx_Open(&db, &look.vt, &s_alloc, &s_alloc) != SZ_OK) goto end;
    opened = 1;
    // The .hts in it (the packs hold it and a few lines of text).
    for (i = 0; i < db.NumFiles && file == 0xFFFFFFFFu; i++)
    {
        size_t n = SzArEx_GetFileNameUtf16(&db, i, NULL), k;
        if (SzArEx_IsDir(&db, i) || n < 6 || n > nameSize) continue;
        free(wname);
        wname = (UInt16 *)malloc(n * sizeof(UInt16));
        if (!wname) goto end;
        SzArEx_GetFileNameUtf16(&db, i, wname);
        for (k = 0; k < n; k++) name[k] = wname[k] < 0x80 ? (char)wname[k] : '_';
        if (n >= 5 && !_stricmp(name + n - 5, ".hts")) file = i;
    }
    if (file == 0xFFFFFFFFu) goto end;
    // Written under the pack's own name from the start: the archive's ("<ROM>_HIRESTEXTURES.hts")
    // may be a pack already there and in use - a game running with it could not have it
    // replaced, and the new one was left a .part (Smash Remix Ultimate over the user's own).
    HiresPackFileName(&s_job, name, nameSize);
    folder = db.FileToFolder[file];
    {
        // One coder, LZMA2 or LZMA, over one packed stream (what 7-Zip makes of a pack).
        const Byte *data = db.db.CodersData + db.db.FoCodersOffsets[folder];
        CSzFolder f;
        CSzData sd;
        UInt64 fileStart = db.UnpackPositions[file] - db.UnpackPositions[db.FolderToFile[folder]];
        UInt64 fileSize = SzArEx_GetFileSize(&db, file), outPos = 0, written = 0;
        // Where its packed stream starts (SzArEx_GetFolderStreamPos is declared, not defined, in SDK 19.00).
        UInt32 packIndex = db.db.FoStartPackStreamIndex[folder], crc = CRC_INIT_VAL;
        UInt64 packPos = db.dataPos + db.db.PackPositions[packIndex];
        UInt64 packLeft = db.db.PackPositions[packIndex + 1] - db.db.PackPositions[packIndex];
        size_t inPos = 0, inLen = 0;
        sd.Data = data;
        sd.Size = db.db.FoCodersOffsets[folder + 1] - db.db.FoCodersOffsets[folder];
        if (SzGetNextFolderItem(&f, &sd) != SZ_OK || f.NumCoders != 1 || f.NumPackStreams != 1) goto end;
        if (f.Coders[0].MethodID == 0x21 && f.Coders[0].PropsSize == 1) lzma2 = 1;
        else if (f.Coders[0].MethodID != 0x30101 || f.Coders[0].PropsSize != 5) goto end;
        // FAT32 holds files under 4 GB; the space for it, the archive still there.
        if (fileSize >= 0xFFFFFFFFull) { err = HPERR_TOOBIG; goto end; }
        if (!space_for(dir, fileSize / 4)) { err = HPERR_SPACE; goto end; }   // about what it comes to as DXT
        InterlockedExchange(&s_doneKb, 0);
        InterlockedExchange(&s_totalKb, (LONG)(fileSize >> 10));
        inBuf = (Byte *)malloc(UNPACK_IN);
        outBuf = (Byte *)malloc(UNPACK_OUT);
        if (!inBuf || !outBuf) goto end;
        if (lzma2)
        {
            Lzma2Dec_Construct(&lz2);
            if (Lzma2Dec_Allocate(&lz2, data[f.Coders[0].PropsOffset], &s_alloc) != SZ_OK) goto end;
            Lzma2Dec_Init(&lz2);
        }
        else
        {
            LzmaDec_Construct(&lz);
            if (LzmaDec_Allocate(&lz, data + f.Coders[0].PropsOffset, 5, &s_alloc) != SZ_OK) goto end;
            LzmaDec_Init(&lz);
        }
        decoder = 1;
        _snprintf(path, sizeof(path) - 1, "%s\\%s", dir, name);
        path[sizeof(path) - 1] = 0;
        _snprintf(part, sizeof(part) - 1, "%s.part", path);
        part[sizeof(part) - 1] = 0;
        out = fopen(part, "wb");
        if (!out || fseek(in, (long)packPos, SEEK_SET)) { err = HPERR_FILE; goto end; }
        setvbuf(out, NULL, _IOFBF, UNPACK_WRITE);
        sh.out = out;
        sh.state = SH_HEADER;
        sh.err = HPERR_ARCHIVE;
        sh.need = 16;
        sh.indexPos = sh.recStart = sh.written = 0;
        sh.converted = 0;
        while (written < fileSize)
        {
            SizeT outLen = UNPACK_OUT, srcLen;
            ELzmaStatus status;
            SRes res;
            if (s_cancel) { err = HPERR_CANCELLED; goto end; }
            if (inPos == inLen && packLeft)
            {
                inLen = fread(inBuf, 1, packLeft < UNPACK_IN ? (size_t)packLeft : UNPACK_IN, in);
                if (!inLen) { err = HPERR_FILE; goto end; }
                packLeft -= inLen;
                inPos = 0;
            }
            srcLen = inLen - inPos;
            res = lzma2 ? Lzma2Dec_DecodeToBuf(&lz2, outBuf, &outLen, inBuf + inPos, &srcLen, LZMA_FINISH_ANY, &status)
                        : LzmaDec_DecodeToBuf(&lz, outBuf, &outLen, inBuf + inPos, &srcLen, LZMA_FINISH_ANY, &status);
            inPos += srcLen;
            if (res != SZ_OK) goto end;
            {
                // The part of what came out that is the .hts (the folder may hold other files before it).
                UInt64 lo = outPos, hi = outPos + outLen;
                if (hi > fileStart && lo < fileStart + fileSize)
                {
                    UInt64 a = lo > fileStart ? lo : fileStart, b = hi < fileStart + fileSize ? hi : fileStart + fileSize;
                    const Byte *p = outBuf + (size_t)(a - lo);
                    size_t n = (size_t)(b - a);
                    if (!shrink_feed(&sh, p, n)) { err = sh.err; goto end; }
                    crc = CrcUpdate(crc, p, n);
                    written += n;
                    InterlockedExchange(&s_doneKb, (LONG)(written >> 10));
                }
                outPos = hi;
            }
            if (!outLen && !srcLen && (inPos == inLen && !packLeft)) break;   // nothing more comes
        }
        if (written != fileSize) goto end;
        if (!shrink_finish(&sh)) { err = sh.err; goto end; }
        H64_INFO("[hirespack] %s: %u of %u textures to DXT, %u MB from %u MB", name, sh.converted, (u32)sh.oldOff.size(),
                 (u32)(sh.written >> 20), (u32)(fileSize >> 20));
        if (SzBitWithVals_Check(&db.CRCs, file) && db.CRCs.Vals[file] != CRC_GET_DIGEST(crc))
        {
            H64_INFO("[hirespack] %s: CRC mismatch", name);
            goto end;
        }
        if (fclose(out)) { out = NULL; err = HPERR_SPACE; goto end; }
        out = NULL;
        DeleteFileA(path);   // an older copy of it
        if (!MoveFileA(part, path)) { err = HPERR_FILE; goto end; }
        ok = 1;
    }
end:
    if (out) { fclose(out); DeleteFileA(part); }
    if (decoder)
    {
        if (lzma2) Lzma2Dec_Free(&lz2, &s_alloc);
        else LzmaDec_Free(&lz, &s_alloc);
    }
    if (opened) SzArEx_Free(&db, &s_alloc);
    free(look.buf);
    free(wname);
    free(inBuf);
    free(outBuf);
    fclose(in);
    return ok ? 1 : fail(err);
}

// ---- PNG packs made a .hts ----
// GLideN64's (and Rice's) other pack form: one PNG a texture, named
//   <ROM name>#<texture CRC>#<format>#<size>[#<palette CRC>]_<kind>.png
// kind "all", "ciByRGBA" or "allciByRGBA": the picture with its alpha; "rgb": the
// colours, its alpha in the "_a" file beside it. They are made one .hts here, as
// DXT (see the shrink above), keyed (palette CRC << 32) | texture CRC as the
// reader looks them up: from a folder of the game's (the user's own, at any
// depth) or straight out of a .zip of them (a catalog pack, nothing unpacked).
#define PNG_MAX_PIXELS (4096u * 2048u)   // decoded at most (32 MB); larger ones are skipped
#define PNG_MAX_SIDE 1024u               // kept at most (the renderer's own limit, HTS_MAX_SIDE)

struct PngName { u64 key; u32 formatSize; int rgb; };

// What a file's name says (0: not a texture of a pack, or an "_a" alpha file).
static int png_name(const char *path, PngName *pn)
{
    const char *base = path, *p, *h, *us;
    char kind[24];
    unsigned tex = 0, fmt = 0, siz = 0, pal = 0;
    size_t n;
    for (p = path; *p; p++)
        if (*p == '\\' || *p == '/') base = p + 1;
    h = strchr(base, '#');
    us = h ? strrchr(h, '_') : NULL;
    if (!us) return 0;
    n = strlen(us);
    if (n < 6 || _stricmp(us + n - 4, ".png") || n - 5 >= sizeof(kind)) return 0;
    memcpy(kind, us + 1, n - 5);
    kind[n - 5] = 0;
    if (sscanf(h, "#%8X#%X#%X#%8X", &tex, &fmt, &siz, &pal) < 3) return 0;
    if (!_stricmp(kind, "rgb")) pn->rgb = 1;
    else if (!_stricmp(kind, "all") || !_stricmp(kind, "ciByRGBA") || !_stricmp(kind, "allciByRGBA")) pn->rgb = 0;
    else return 0;
    pn->key = (u64)pal << 32 | tex;
    pn->formatSize = (siz & 3) << 8 | (fmt & 7);
    return 1;
}

// The .hts being written: its records, then the index of them.
struct PngOut
{
    FILE *out;
    u64 written;
    std::vector<u8> index, dxt;
    std::set<u64> seen;
    u32 count;
};

static int png_out_write(PngOut *o, const void *p, size_t n)
{
    if (fwrite(p, 1, n, o->out) != n) return 0;
    o->written += n;
    return 1;
}

static int png_out_begin(PngOut *o, FILE *out)
{
    u8 hdr[16];
    o->out = out;
    o->written = 0;
    o->count = 0;
    wr32le(hdr, 0x08000000u);   // the config word of GLideN64's packs (the reader does not use it)
    wr32le(hdr + 4, 0);
    wr64le(hdr + 8, 0);         // indexPos: set at the end
    return png_out_write(o, hdr, 16);
}

// One picture (RGBA, decoded) as a record; a key already there is left as it was.
static int png_out_add(PngOut *o, const PngName *pn, u8 *rgba, u32 w, u32 h)
{
    u8 rec[23], e[16], *px = rgba, *small = NULL;
    u32 i, alpha = 0, size;
    if (o->seen.count(pn->key)) return 1;
    if (w > PNG_MAX_SIDE || h > PNG_MAX_SIDE)
    {
        int sw, sh;
        h64_image_fit((int)w, (int)h, PNG_MAX_SIDE, PNG_MAX_SIDE, &sw, &sh);
        small = h64_image_scale(rgba, (int)w, (int)h, sw, sh);
        if (!small) return 1;
        px = small; w = (u32)sw; h = (u32)sh;
    }
    for (i = 3; i < w * h * 4 && !alpha; i += 4) alpha = px[i] != 255;
    size = ((w + 3) / 4) * ((h + 3) / 4) * (alpha ? 16 : 8);
    o->dxt.resize(size);
    dxt_encode(px, w, h, alpha, &o->dxt[0]);
    free(small);
    wr32le(rec, w);
    wr32le(rec + 4, h);
    wr32le(rec + 8, alpha ? HTS_DXT5 : HTS_DXT1);
    wr16le(rec + 12, HTS_RGBA);
    wr16le(rec + 14, HTS_UBYTE);
    rec[16] = 1;   // a hi-res texture
    wr16le(rec + 17, pn->formatSize);
    wr32le(rec + 19, size);
    wr64le(e, pn->key);
    wr64le(e + 8, o->written);
    if (!png_out_write(o, rec, 23) || !png_out_write(o, &o->dxt[0], size)) return 0;
    o->index.insert(o->index.end(), e, e + 16);
    o->seen.insert(pn->key);
    o->count++;
    return 1;
}

static int png_out_finish(PngOut *o)
{
    u8 n[4], pos[8];
    wr32le(n, o->count);
    wr64le(pos, o->written);
    if (!png_out_write(o, n, 4) || (o->count && !png_out_write(o, &o->index[0], o->index.size()))) return 0;
    return !fseek(o->out, 8, SEEK_SET) && fwrite(pos, 1, 8, o->out) == 8;
}

// A picture from its file's bytes (and its alpha file's, for an "_rgb" one) into the .hts.
static int png_take(PngOut *o, const PngName *pn, const u8 *data, u32 size, const u8 *alphaData, u32 alphaSize)
{
    int w = 0, h = 0, aw = 0, ah = 0, ok;
    u8 *rgba = h64_png_decode(data, size, &w, &h, PNG_MAX_PIXELS), *a;
    if (!rgba) return 1;   // not a PNG it can read: skipped
    if (pn->rgb)
    {
        u32 i;
        a = alphaData ? h64_png_decode(alphaData, alphaSize, &aw, &ah, PNG_MAX_PIXELS) : NULL;
        for (i = 0; i < (u32)w * h; i++) rgba[i * 4 + 3] = a && aw == w && ah == h ? a[i * 4] : 255;
        free(a);
    }
    ok = png_out_add(o, pn, rgba, (u32)w, (u32)h);
    free(rgba);
    return ok;
}

// The output file: <dest>\<file> written as .part, renamed when whole.
static FILE *png_open(const char *dest, const char *file, char *part, size_t partSize)
{
    FILE *f;
    _snprintf(part, partSize - 1, "%s\\%s.part", dest, file);
    part[partSize - 1] = 0;
    f = fopen(part, "wb");
    if (f) setvbuf(f, NULL, _IOFBF, UNPACK_WRITE);
    return f;
}

static int png_close(FILE *f, const char *part, int ok)
{
    char path[256];
    size_t n = strlen(part) - 5;   // without ".part"
    if (fclose(f)) ok = 0;
    if (!ok) { DeleteFileA(part); return 0; }
    memcpy(path, part, n);
    path[n] = 0;
    DeleteFileA(path);
    return MoveFileA(part, path) != 0;
}

static u8 *read_whole(const char *path, u32 *size)
{
    FILE *f = fopen(path, "rb");
    u8 *d = NULL;
    long n;
    *size = 0;
    if (!f) return NULL;
    if (!fseek(f, 0, SEEK_END) && (n = ftell(f)) > 0 && n < (64L << 20) && !fseek(f, 0, SEEK_SET) && (d = (u8 *)malloc((size_t)n)) != NULL)
    {
        if (fread(d, 1, (size_t)n, f) == (size_t)n) *size = (u32)n;
        else { free(d); d = NULL; }
    }
    fclose(f);
    return d;
}

// Every .png under dir (at most a few folders deep) and their bytes in all.
static void png_scan(const char *dir, std::vector<std::string> &files, u64 *bytes, int depth)
{
    char pattern[300];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    _snprintf(pattern, sizeof(pattern) - 1, "%s\\*", dir);
    pattern[sizeof(pattern) - 1] = 0;
    h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do
    {
        std::string path = std::string(dir) + "\\" + fd.cFileName;
        size_t n = strlen(fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        {
            if (strcmp(fd.cFileName, ".") && strcmp(fd.cFileName, "..") && depth < 4) png_scan(path.c_str(), files, bytes, depth + 1);
        }
        else if (n > 4 && !_stricmp(fd.cFileName + n - 4, ".png"))
        {
            files.push_back(path);
            *bytes += fd.nFileSizeLow;
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

int HiresPngPresent(const char *dir)
{
    std::vector<std::string> files;
    u64 bytes = 0;
    char pattern[300];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    // The usual case first (the pictures in the folder itself), then any depth.
    _snprintf(pattern, sizeof(pattern) - 1, "%s\\*#*.png", dir);
    pattern[sizeof(pattern) - 1] = 0;
    h = FindFirstFileA(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) { FindClose(h); return 1; }
    png_scan(dir, files, &bytes, 2);   // two levels below at most here (cheap: the menu asks it)
    return !files.empty();
}

// The folder's PNG pack converted into <dest>\<name>.
static int png_convert_folder(const char *scan, const char *dest, char *name, size_t nameSize)
{
    std::vector<std::string> files;
    std::set<std::string> lower;   // the files there, lower case: an "_rgb" one's "_a" found in it
    u64 bytes = 0, done = 0;
    PngOut o;
    FILE *f;
    char part[256];
    size_t i;
    int ok = 1;
    png_scan(scan, files, &bytes, 0);
    for (i = 0; i < files.size(); i++)
    {
        std::string l = files[i];
        size_t k;
        for (k = 0; k < l.size(); k++) l[k] = (char)tolower((unsigned char)l[k]);
        lower.insert(l);
    }
    InterlockedExchange(&s_doneKb, 0);
    InterlockedExchange(&s_totalKb, (LONG)(bytes >> 10));
    HiresPackFileName(&s_job, name, nameSize);   // its final name (see unpack)
    f = png_open(dest, name, part, sizeof(part));
    if (!f) return fail(HPERR_FILE);
    ok = png_out_begin(&o, f);
    for (i = 0; ok && i < files.size(); i++)
    {
        PngName pn;
        u32 size = 0, asize = 0;
        u8 *data, *adata = NULL;
        if (s_cancel) { png_close(f, part, 0); return fail(HPERR_CANCELLED); }
        if (png_name(files[i].c_str(), &pn) && (data = read_whole(files[i].c_str(), &size)) != NULL)
        {
            if (pn.rgb)
            {
                // Its alpha: the same name, "_a" for "_rgb".
                std::string a = files[i].substr(0, files[i].size() - 8) + "_a.png", al = a;
                size_t k;
                for (k = 0; k < al.size(); k++) al[k] = (char)tolower((unsigned char)al[k]);
                if (lower.count(al)) adata = read_whole(a.c_str(), &asize);
            }
            ok = png_take(&o, &pn, data, size, adata, asize);
            free(data);
            free(adata);
        }
        {
            WIN32_FILE_ATTRIBUTE_DATA fa;
            if (GetFileAttributesExA(files[i].c_str(), GetFileExInfoStandard, &fa)) done += fa.nFileSizeLow;
        }
        InterlockedExchange(&s_doneKb, (LONG)(done >> 10));
    }
    if (ok) ok = png_out_finish(&o);
    H64_INFO("[hirespack] %s: %u of %u PNG files made %s (%u KB)", dest, o.count, (u32)files.size(), name, (u32)(o.written >> 10));
    if (!o.count) { png_close(f, part, 0); return fail(HPERR_NOPNG); }
    if (!png_close(f, part, ok)) return fail(HPERR_SPACE);
    return 1;
}

// ---- A .zip of PNGs (a catalog pack): read in place, no unpacking ----
struct ZipEntry { std::string name; u32 method, csize, usize, local; };

static u32 rd16le(const u8 *p) { return p[0] | p[1] << 8; }

static int zip_list(FILE *f, std::vector<ZipEntry> &out)
{
    u8 tail[65536 + 22], *cd;
    long size, n, k;
    u32 cdSize, cdOff, count, i, pos = 0;
    if (fseek(f, 0, SEEK_END) || (size = ftell(f)) < 22) return 0;
    n = size < (long)sizeof(tail) ? size : (long)sizeof(tail);
    if (fseek(f, size - n, SEEK_SET) || fread(tail, 1, (size_t)n, f) != (size_t)n) return 0;
    for (k = n - 22; k >= 0 && rd32le(tail + k) != 0x06054B50u; k--) {}
    if (k < 0) return 0;
    count = rd16le(tail + k + 10);
    cdSize = rd32le(tail + k + 12);
    cdOff = rd32le(tail + k + 16);
    if (cdOff == 0xFFFFFFFFu || (long)cdOff + (long)cdSize > size || cdSize > (16u << 20)) return 0;   // no ZIP64
    cd = (u8 *)malloc(cdSize ? cdSize : 1);
    if (!cd || fseek(f, (long)cdOff, SEEK_SET) || fread(cd, 1, cdSize, f) != cdSize) { free(cd); return 0; }
    for (i = 0; i < count && pos + 46 <= cdSize && rd32le(cd + pos) == 0x02014B50u; i++)
    {
        ZipEntry e;
        u32 nl = rd16le(cd + pos + 28), el = rd16le(cd + pos + 30), cl = rd16le(cd + pos + 32);
        if (pos + 46 + nl > cdSize) break;
        e.method = rd16le(cd + pos + 10);
        e.csize = rd32le(cd + pos + 20);
        e.usize = rd32le(cd + pos + 24);
        e.local = rd32le(cd + pos + 42);
        e.name.assign((const char *)cd + pos + 46, nl);
        out.push_back(e);
        pos += 46 + nl + el + cl;
    }
    free(cd);
    return 1;
}

static voidpf zip_zalloc(voidpf, uInt items, uInt size) { return malloc((size_t)items * size); }
static void zip_zfree(voidpf, voidpf p) { free(p); }

// An entry's bytes, inflated (deflate) or as stored.
static u8 *zip_read(FILE *f, const ZipEntry *e, u32 *size)
{
    u8 lh[30], *c, *d;
    z_stream z;
    *size = 0;
    if ((e->method != 0 && e->method != 8) || e->usize > (64u << 20) || e->csize > (64u << 20)) return NULL;
    if (fseek(f, (long)e->local, SEEK_SET) || fread(lh, 1, 30, f) != 30 || rd32le(lh) != 0x04034B50u) return NULL;
    if (fseek(f, (long)(e->local + 30 + rd16le(lh + 26) + rd16le(lh + 28)), SEEK_SET)) return NULL;
    c = (u8 *)malloc(e->csize ? e->csize : 1);
    if (!c || fread(c, 1, e->csize, f) != e->csize) { free(c); return NULL; }
    if (e->method == 0) { *size = e->csize; return c; }
    d = (u8 *)malloc(e->usize ? e->usize : 1);
    memset(&z, 0, sizeof(z));
    z.zalloc = zip_zalloc;
    z.zfree = zip_zfree;
    if (!d || inflateInit2(&z, -15) != Z_OK) { free(c); free(d); return NULL; }
    z.next_in = c; z.avail_in = e->csize;
    z.next_out = d; z.avail_out = e->usize;
    if (inflate(&z, Z_FINISH) != Z_STREAM_END) { inflateEnd(&z); free(c); free(d); return NULL; }
    inflateEnd(&z);
    free(c);
    *size = e->usize;
    return d;
}

// A .hts stored or deflated in a .zip, made DXT as it is read (the shrink, as for a .7z).
static int hts_from_zip(FILE *in, const ZipEntry *e, const char *dest, char *name, size_t nameSize)
{
    Shrink sh;
    u8 lh[30], *inBuf = NULL, *outBuf = NULL;
    FILE *out;
    char part[256];
    u32 left = e->csize, done = 0;
    int ok = 0, zinit = 0;
    z_stream z;
    const char *base = e->name.c_str(), *p;
    (void)base; (void)p;
    HiresPackFileName(&s_job, name, nameSize);   // its final name (see unpack)
    if (e->local != 0xFFFFFFFFu &&   // a .hts file of its own: read from where it is
        ((e->method != 0 && e->method != 8) || fseek(in, (long)e->local, SEEK_SET) || fread(lh, 1, 30, in) != 30 || rd32le(lh) != 0x04034B50u ||
         fseek(in, (long)(e->local + 30 + rd16le(lh + 26) + rd16le(lh + 28)), SEEK_SET)))
        return fail(HPERR_ARCHIVE);
    if (!space_for(dest, e->usize / 4)) return fail(HPERR_SPACE);
    out = png_open(dest, name, part, sizeof(part));
    if (!out) return fail(HPERR_FILE);
    sh.out = out;
    sh.state = SH_HEADER;
    sh.err = HPERR_ARCHIVE;
    sh.need = 16;
    sh.indexPos = sh.recStart = sh.written = 0;
    sh.converted = 0;
    InterlockedExchange(&s_doneKb, 0);
    InterlockedExchange(&s_totalKb, (LONG)(e->csize >> 10));
    inBuf = (u8 *)malloc(UNPACK_IN);
    outBuf = (u8 *)malloc(UNPACK_OUT);
    memset(&z, 0, sizeof(z));
    z.zalloc = sh_zalloc;
    z.zfree = sh_zfree;
    if (!inBuf || !outBuf || (e->method == 8 && inflateInit2(&z, -15) != Z_OK)) goto end;
    zinit = e->method == 8;
    while (left)
    {
        size_t n = fread(inBuf, 1, left < UNPACK_IN ? left : UNPACK_IN, in);
        if (s_cancel) { sh.err = HPERR_CANCELLED; goto end; }
        if (!n) goto end;
        left -= (u32)n;
        done += (u32)n;
        if (!zinit) { if (!shrink_feed(&sh, inBuf, n)) goto end; }
        else
        {
            z.next_in = inBuf;
            z.avail_in = (uInt)n;
            while (z.avail_in)
            {
                int r;
                z.next_out = outBuf;
                z.avail_out = UNPACK_OUT;
                r = inflate(&z, Z_NO_FLUSH);
                if (r != Z_OK && r != Z_STREAM_END) goto end;
                if (!shrink_feed(&sh, outBuf, UNPACK_OUT - z.avail_out)) goto end;
                if (r == Z_STREAM_END) break;
            }
        }
        InterlockedExchange(&s_doneKb, (LONG)(done >> 10));
    }
    ok = shrink_finish(&sh);
    H64_INFO("[hirespack] %s: %u of %u textures to DXT, %u MB from %u MB", name, sh.converted, (u32)sh.oldOff.size(),
             (u32)(sh.written >> 20), e->usize >> 20);
end:
    if (zinit) inflateEnd(&z);
    free(inBuf);
    free(outBuf);
    if (!png_close(out, part, ok)) return fail(ok ? HPERR_SPACE : sh.err);
    return 1;
}

// A .hts file (the user's own pack) made DXT into dest (the shrink, as for one in a .zip).
static int hts_from_file(const char *path, const char *dest, char *name, size_t nameSize)
{
    ZipEntry e;
    FILE *in = fopen(path, "rb");
    long size;
    int ok;
    if (!in) return fail(HPERR_FILE);
    if (fseek(in, 0, SEEK_END) || (size = ftell(in)) <= 16 || fseek(in, 0, SEEK_SET)) { fclose(in); return fail(HPERR_ARCHIVE); }
    e.name = path;
    e.method = 0;
    e.csize = e.usize = (u32)size;
    e.local = 0xFFFFFFFFu;
    ok = hts_from_zip(in, &e, dest, name, nameSize);
    fclose(in);
    return ok;
}

static int png_convert_zip(const char *archive, const char *dest, char *name, size_t nameSize)
{
    std::vector<ZipEntry> list;
    std::map<std::string, size_t> byName;   // lower-case names: an "_rgb" one's "_a"
    u64 bytes = 0, done = 0;
    PngOut o;
    FILE *in = fopen(archive, "rb"), *f;
    char part[256];
    size_t i;
    int ok;
    if (!in) return fail(HPERR_FILE);
    if (!zip_list(in, list)) { fclose(in); return fail(HPERR_ARCHIVE); }
    for (i = 0; i < list.size(); i++)
    {
        // A .hts in it instead of PNGs: that one, through the DXT shrink.
        size_t nl = list[i].name.size();
        if (nl > 4 && !_stricmp(list[i].name.c_str() + nl - 4, ".hts"))
        {
            int ok = hts_from_zip(in, &list[i], dest, name, nameSize);
            fclose(in);
            return ok;
        }
    }
    for (i = 0; i < list.size(); i++)
    {
        std::string l = list[i].name;
        size_t k;
        for (k = 0; k < l.size(); k++) l[k] = (char)tolower((unsigned char)l[k]);
        byName[l] = i;
        bytes += list[i].csize;
    }
    InterlockedExchange(&s_doneKb, 0);
    InterlockedExchange(&s_totalKb, (LONG)(bytes >> 10));
    HiresPackFileName(&s_job, name, nameSize);   // its final name (see unpack)
    f = png_open(dest, name, part, sizeof(part));
    if (!f) { fclose(in); return fail(HPERR_FILE); }
    ok = png_out_begin(&o, f);
    for (i = 0; ok && i < list.size(); i++)
    {
        PngName pn;
        u32 size = 0, asize = 0;
        u8 *data, *adata = NULL;
        if (s_cancel) { fclose(in); png_close(f, part, 0); return fail(HPERR_CANCELLED); }
        if (png_name(list[i].name.c_str(), &pn) && (data = zip_read(in, &list[i], &size)) != NULL)
        {
            if (pn.rgb)
            {
                std::string a = list[i].name.substr(0, list[i].name.size() - 8) + "_a.png";
                std::map<std::string, size_t>::const_iterator it;
                size_t k;
                for (k = 0; k < a.size(); k++) a[k] = (char)tolower((unsigned char)a[k]);
                if ((it = byName.find(a)) != byName.end()) adata = zip_read(in, &list[it->second], &asize);
            }
            ok = png_take(&o, &pn, data, size, adata, asize);
            free(data);
            free(adata);
        }
        done += list[i].csize;
        InterlockedExchange(&s_doneKb, (LONG)(done >> 10));
    }
    fclose(in);
    if (ok) ok = png_out_finish(&o);
    H64_INFO("[hirespack] %s: %u of %u files made %s (%u KB)", archive, o.count, (u32)list.size(), name, (u32)(o.written >> 10));
    if (!o.count) { png_close(f, part, 0); return fail(HPERR_NOPNG); }
    if (!png_close(f, part, ok)) return fail(HPERR_SPACE);
    return 1;
}

static void install_name(void);   // below: the .hts named after its pack, made the one in use

static DWORD WINAPI png_thread(LPVOID)
{
    DWORD t0 = GetTickCount();
    InterlockedExchange(&s_state, HPACK_CONVERT);
    if (!png_convert_folder(s_dest, s_dest, s_file, sizeof(s_file))) return 0;
    install_name();
    H64_INFO("[hirespack] converted in %.1f s: %s\\%s", (GetTickCount() - t0) / 1000.0f, s_dest, s_file);
    InterlockedExchange(&s_state, HPACK_DONE);
    return 0;
}


// ---- The catalog's release information (the Packs screen, START in the ROM list) ----
// What GitHub says of each pack's latest release, asked for one pack at a time (the API
// allows 60 questions an hour without an account) and kept for the session.
static HiresPackInfo s_info[PACK_COUNT];
static volatile LONG s_infoBusy;
static int s_infoWant = -1;

int HiresPackTotal(void) { return PACK_COUNT; }

// The games of the catalog by their usual names (the Packs screen's headings), from their
// ROM headers' names; another's own name when it is not here.
const char *HiresGameName(const char *romName)
{
    static const char *const names[][2] = {
        { "Banjo-Kazooie", "Banjo-Kazooie" }, { "BANJO TOOIE", "Banjo-Tooie" }, { "GOLDENEYE", "GoldenEye 007" },
        { "MARIOKART64", "Mario Kart 64" }, { "Resident Evil II", "Resident Evil 2" }, { "SMASH BROTHERS", "Super Smash Bros." },
        { "SMASH REMIX", "Smash Remix" }, { "SUPER MARIO 64", "Super Mario 64" },
        { "THE LEGEND OF ZELDA", "The Legend of Zelda: Ocarina of Time" }, { "Turok 3- Shadow of O", "Turok 3: Shadow of Oblivion" },
        { "ZELDA MAJORA'S MASK", "The Legend of Zelda: Majora's Mask" }, { "F-ZERO X", "F-Zero X" }, { "CASTLEVANIA", "Castlevania" },
        { "CASTLEVANIA2", "Castlevania: Legacy of Darkness" }, { "CONKER BFD", "Conker's Bad Fur Day" },
        { "Diddy Kong Racing", "Diddy Kong Racing" }, { "DUKE NUKEM", "Duke Nukem 64" }, { "HARVESTMOON64", "Harvest Moon 64" },
        { "PAPER MARIO", "Paper Mario" }, { "Shadowman", "Shadow Man" }, { "TSUMI TO BATSU", "Sin and Punishment" },
        { "STARFOX64", "Star Fox 64" }, { "AEROGAUGE", "AeroGauge" },
    };
    int i;
    for (i = 0; i < (int)(sizeof(names) / sizeof(names[0])); i++)
        if (!strcmp(romName, names[i][0])) return names[i][1];
    return romName;
}
const HiresPackEntry *HiresPackAt(int i) { return i >= 0 && i < PACK_COUNT ? &s_packs[i] : NULL; }

// A JSON string's value after `key` ("key":"...") from `from`, its escapes undone (\n kept).
static int json_str(const char *from, const char *key, char *out, size_t size)
{
    char k[48];
    const char *p;
    size_t n = 0;
    _snprintf(k, sizeof(k) - 1, "\"%s\":\"", key);
    k[sizeof(k) - 1] = 0;
    out[0] = 0;
    if (!from || !(p = strstr(from, k))) return 0;
    for (p += strlen(k); *p && *p != '"' && n + 1 < size; p++)
    {
        char ch = *p;
        if (ch == '\\' && p[1])
        {
            p++;
            ch = *p == 'n' ? '\n' : *p == 'r' ? '\r' : *p == 't' ? ' ' : *p == 'u' ? '?' : *p;
            if (*p == 'u') { int j; for (j = 0; j < 4 && p[1]; j++) p++; }
        }
        if (ch == '\r') continue;
        out[n++] = ch;
    }
    out[n] = 0;
    return 1;
}

static unsigned json_num_last(const char *from, const char *to, const char *key)
{
    const char *q = from, *hit = NULL;
    size_t kl = strlen(key);
    while ((q = strstr(q, key)) != NULL && q < to) { hit = q; q += kl; }
    return hit ? (unsigned)strtoul(hit + kl, NULL, 10) : 0;
}

// Release notes made plain: markdown marks and links' targets dropped, blank lines folded.
static void notes_plain(const char *in, char *out, size_t size)
{
    size_t n = 0;
    int nl = 1, paren = 0;
    char last = 0;
    for (; *in && n + 1 < size; in++)
    {
        char ch = *in, before = last;
        last = ch;
        if (ch == '(' && before == ']') { paren = 1; continue; }
        if (paren) { if (ch == ')') paren = 0; continue; }
        if (ch == '#' || ch == '*' || ch == '`' || ch == '[' || ch == ']' || ch == '>' || ch == '_') continue;
        if (ch == '\n') { if (nl >= 2) continue; nl++; }
        else if (ch == ' ' && nl) continue;
        else if (ch == '-' && nl) { out[n++] = (char)0xE2; if (n + 2 < size) { out[n++] = (char)0x80; out[n++] = (char)0xA2; } continue; }   // a bullet
        else nl = 0;
        out[n++] = ch;
    }
    while (n && (out[n - 1] == '\n' || out[n - 1] == ' ')) n--;
    out[n] = 0;
}

static DWORD WINAPI info_thread(LPVOID)
{
    for (;;)
    {
        int i = s_infoWant;
        char path[160], url[1024];
        u8 *json = NULL;
        u32 len = 0;
        unsigned size = 0;
        int st;
        HiresPackInfo *in;
        if (i < 0 || i >= PACK_COUNT || s_info[i].state != HPINFO_LOADING) break;
        in = &s_info[i];
        _snprintf(path, sizeof(path) - 1, "/repos/%s/%s/releases/latest", s_packs[i].owner, s_packs[i].repo);
        path[sizeof(path) - 1] = 0;
        st = NetStart(0) ? NetGet("api.github.com", path, 1, &json, &len, 2u << 20, NULL, 0) : 0;
        if (st == 200 && json && find_asset((const char *)json, s_packs[i].asset, url, sizeof(url), &size))
        {
            const char *j = (const char *)json, *u = strstr(j, url), *slash = strrchr(url, '/');
            json_str(j, "tag_name", in->version, sizeof(in->version));
            json_str(j, "published_at", in->date, sizeof(in->date));
            if (strlen(in->date) > 10) in->date[10] = 0;   // the day (2026-05-19)
            strncpy(in->file, slash ? slash + 1 : url, sizeof(in->file) - 1);
            in->file[sizeof(in->file) - 1] = 0;
            in->size = size;
            in->downloads = u ? json_num_last(j, u, "\"download_count\":") : 0;
            {
                static char body[4096];
                json_str(j, "body", body, sizeof(body));
                notes_plain(body, in->notes, sizeof(in->notes));
            }
            in->state = HPINFO_OK;
        }
        else
        {
            H64_INFO("[hirespack] %s: release information not found (HTTP %d)", s_packs[i].repo, st);
            in->state = HPINFO_FAILED;
        }
        free(json);
        if (s_infoWant == i) break;   // nothing else asked meanwhile
    }
    InterlockedExchange(&s_infoBusy, 0);
    return 0;
}

int HiresPackInfoGet(int i, HiresPackInfo *out, int ask)
{
    if (i < 0 || i >= PACK_COUNT) return HPINFO_NONE;
    if (!s_packs[i].owner[0]) return HPINFO_NONE;   // a direct download: no release to ask about
    if ((ask && s_info[i].state == HPINFO_NONE) || (ask == 2 && s_info[i].state == HPINFO_FAILED))
    {
        // Asked for: one thread asks GitHub (the last pack asked for, when it is done with one).
        s_info[i].state = HPINFO_LOADING;
        s_infoWant = i;
        if (!InterlockedExchange(&s_infoBusy, 1))
        {
            HANDLE t = CreateThread(NULL, 256 * 1024, info_thread, NULL, CREATE_SUSPENDED, NULL);
            if (!t) { s_info[i].state = HPINFO_FAILED; InterlockedExchange(&s_infoBusy, 0); }
            else { XSetThreadProcessor(t, 3); ResumeThread(t); CloseHandle(t); }
        }
    }
    else if (s_info[i].state == HPINFO_LOADING && !s_infoBusy) s_info[i].state = HPINFO_NONE;   // asked while another ended: again
    if (out) *out = s_info[i];
    return s_info[i].state;
}


// ---- The packs' pictures (the Packs screen) ----
// Downloaded and decoded (small) one at a time on a thread, the last one asked for first;
// the screen makes each a texture and hands the pixels back (HiresPackImageDone).
#define PACK_IMAGE_W 480
#define PACK_IMAGE_H 270
struct PackImage { volatile LONG state; u8 *rgba; int w, h; };
static PackImage s_img[PACK_COUNT];
static volatile LONG s_imgBusy;
static int s_imgWant = -1;

static DWORD WINAPI image_thread(LPVOID)
{
    for (;;)
    {
        int i = s_imgWant;
        const char *url, *h, *slash;
        char host[128];
        u8 *data = NULL;
        u32 len = 0;
        int st = 0;
        PackImage *im;
        if (i < 0 || i >= PACK_COUNT || s_img[i].state != HPINFO_LOADING) break;
        im = &s_img[i];
        url = s_packs[i].image;
        h = url && !strncmp(url, "https://", 8) ? url + 8 : NULL;
        slash = h ? strchr(h, '/') : NULL;
        if (slash && (size_t)(slash - h) < sizeof(host) && NetStart(0))
        {
            memcpy(host, h, slash - h);
            host[slash - h] = 0;
            st = NetGet(host, slash, 1, &data, &len, 4u << 20, NULL, 0);
        }
        if (st == 200 && data)
        {
            int w = 0, hh = 0;
            u8 *px = UiDecodeImage(data, (long)len, PACK_IMAGE_W, PACK_IMAGE_H, &w, &hh);
            if (px) { im->rgba = px; im->w = w; im->h = hh; }
            InterlockedExchange(&im->state, px ? HPINFO_OK : HPINFO_FAILED);
        }
        else
        {
            H64_INFO("[hirespack] %s: picture not loaded (HTTP %d)", s_packs[i].title, st);
            InterlockedExchange(&im->state, HPINFO_FAILED);
        }
        free(data);
        if (s_imgWant == i) break;
    }
    InterlockedExchange(&s_imgBusy, 0);
    return 0;
}

int HiresPackImage(int i, const unsigned char **rgba, int *w, int *h)
{
    if (i < 0 || i >= PACK_COUNT || !s_packs[i].image) return HPINFO_FAILED;
    if (s_img[i].state == HPINFO_NONE)
    {
        InterlockedExchange(&s_img[i].state, HPINFO_LOADING);
        s_imgWant = i;
        if (!InterlockedExchange(&s_imgBusy, 1))
        {
            HANDLE t = CreateThread(NULL, 256 * 1024, image_thread, NULL, CREATE_SUSPENDED, NULL);
            if (!t) { InterlockedExchange(&s_img[i].state, HPINFO_FAILED); InterlockedExchange(&s_imgBusy, 0); }
            else { XSetThreadProcessor(t, 3); ResumeThread(t); CloseHandle(t); }
        }
    }
    else if (s_img[i].state == HPINFO_LOADING && !s_imgBusy) InterlockedExchange(&s_img[i].state, HPINFO_NONE);   // asked while another ended
    else if (s_img[i].state == HPINFO_LOADING) s_imgWant = i;   // this one next
    if (s_img[i].state == HPINFO_OK && rgba) { *rgba = s_img[i].rgba; *w = s_img[i].w; *h = s_img[i].h; }
    return s_img[i].state;
}

void HiresPackImageDone(int i)
{
    // The screen made its texture: the pixels go (the state stays: not loaded again).
    if (i < 0 || i >= PACK_COUNT || s_img[i].state != HPINFO_OK) return;
    free(s_img[i].rgba);
    s_img[i].rgba = NULL;
}

void HiresPackImageForget(void)
{
    // The screen closed and its textures went: loaded again the next time.
    int i;
    for (i = 0; i < PACK_COUNT; i++)
        if (s_img[i].state == HPINFO_OK && !s_img[i].rgba) s_img[i].state = HPINFO_NONE;
}

// ---- Installed packs: their names, the one in use ----
// A pack is installed as "<title> - <author>.hts" in its game's folder (names a FAT32 drive
// takes, 42 characters at most as the console's file calls do), so several of a game
// can be told apart; <folder>\active.txt names the one used ("-": none, HD off for it).
void HiresPackFileName(const HiresPackEntry *e, char *out, size_t size)
{
    char raw[96], clean[96];
    size_t n;
    if (!size) return;
    if (!e->author[0]) _snprintf(raw, sizeof(raw) - 1, "%s", e->title);   // the game's own PNG pack
    else _snprintf(raw, sizeof(raw) - 1, "%s - %s", e->title, e->author);
    raw[sizeof(raw) - 1] = 0;
    HiresFolderName(raw, clean, sizeof(clean));
    n = strlen(clean);
    if (n > 38) n = 38;   // + ".hts"
    while (n && (clean[n - 1] == ' ' || clean[n - 1] == '.')) n--;
    _snprintf(out, size - 1, "%.*s.hts", (int)n, clean);
    out[size - 1] = 0;
}

int HiresActiveRead(const char *dir, char *file, size_t size)
{
    char path[256], line[128];
    FILE *f;
    size_t n;
    if (size) file[0] = 0;
    _snprintf(path, sizeof(path) - 1, "%s\\active.txt", dir);
    path[sizeof(path) - 1] = 0;
    if (!(f = fopen(path, "r"))) return 0;
    line[0] = 0;
    if (!fgets(line, sizeof(line), f)) line[0] = 0;
    fclose(f);
    n = strlen(line);
    while (n && (line[n - 1] == '\n' || line[n - 1] == '\r' || line[n - 1] == ' ')) line[--n] = 0;
    if (!strcmp(line, "-")) return -1;
    if (!n) return 0;
    strncpy(file, line, size - 1);
    file[size - 1] = 0;
    return 1;
}

void HiresActiveWrite(const char *dir, const char *file)
{
    char path[256];
    FILE *f;
    _snprintf(path, sizeof(path) - 1, "%s\\active.txt", dir);
    path[sizeof(path) - 1] = 0;
    if (!(f = fopen(path, "w"))) return;
    fprintf(f, "%s\n", file && file[0] ? file : "-");
    fclose(f);
}

// The .hts just made (s_file in s_dest) renamed after its pack, and made the one in use.
static void install_name(void)
{
    char want[64], from[256], to[256];
    HiresPackFileName(&s_job, want, sizeof(want));
    if (_stricmp(want, s_file))
    {
        _snprintf(from, sizeof(from) - 1, "%s\\%s", s_dest, s_file);
        from[sizeof(from) - 1] = 0;
        _snprintf(to, sizeof(to) - 1, "%s\\%s", s_dest, want);
        to[sizeof(to) - 1] = 0;
        DeleteFileA(to);   // an older copy of the same pack
        if (MoveFileA(from, to)) { strncpy(s_file, want, sizeof(s_file) - 1); s_file[sizeof(s_file) - 1] = 0; }
    }
    HiresActiveWrite(s_dest, s_file);
}

// ---- The worker ----
static DWORD WINAPI pack_thread(LPVOID)
{
    char path[160], url[1024], archive[256];
    u8 *json = NULL;
    u32 len = 0;
    unsigned size = 0;
    int st;
    FILE *f;
    DWORD t0, t1;
    if (!NetStart(0)) return fail(HPERR_NETWORK);
    if (!s_job.owner[0])
    {
        // A direct download (Emulation King, n64textures.com): its URL and size are the catalog's.
        strncpy(url, s_job.repo, sizeof(url) - 1);
        url[sizeof(url) - 1] = 0;
        size = s_job.size;
    }
    else
    {
        _snprintf(path, sizeof(path) - 1, "/repos/%s/%s/releases/latest", s_job.owner, s_job.repo);
        path[sizeof(path) - 1] = 0;
        st = NetGet("api.github.com", path, 1, &json, &len, 2u << 20, NULL, 0);
        if (st != 200 || !json)
        {
            H64_INFO("[hirespack] %s: release not found (HTTP %d)", s_job.repo, st);
            free(json);
            return fail(st ? HPERR_NOPACK : HPERR_NETWORK);
        }
        if (!find_asset((const char *)json, s_job.asset, url, sizeof(url), &size))
        {
            H64_INFO("[hirespack] %s: no .hts in its latest release", s_job.repo);
            free(json);
            return fail(HPERR_NOPACK);
        }
    }
    free(json);
    if (strncmp(url, "https://", 8) || !strchr(url + 8, '/')) return fail(HPERR_NOPACK);
    if (!space_for(s_dest, size)) return fail(HPERR_SPACE);
    H64_INFO("[hirespack] %s: %s (%u bytes)", s_job.repo, url, size);
    // Downloaded beside where it goes, unpacked, then dropped.
    CreateDirectoryA(s_dest, NULL);
    _snprintf(archive, sizeof(archive) - 1, "%s\\download.part", s_dest);
    archive[sizeof(archive) - 1] = 0;
    f = fopen(archive, "wb");
    if (!f) return fail(HPERR_FILE);
    s_dlNet.got = 0;
    s_dlNet.total = (long)size;
    InterlockedExchange(&s_state, HPACK_DOWNLOAD);
    t0 = GetTickCount();
    {
        char host[128];
        const char *h = url + 8, *slash = strchr(h, '/');
        size_t hl = (size_t)(slash - h);
        if (hl >= sizeof(host)) { fclose(f); DeleteFileA(archive); return fail(HPERR_NOPACK); }
        memcpy(host, h, hl);
        host[hl] = 0;
        st = NetDownload(host, slash, f, &len, &s_dlNet, &s_cancel);
    }
    if (fclose(f)) st = 0;
    if (st != 200)
    {
        DeleteFileA(archive);
        if (s_cancel) return fail(HPERR_CANCELLED);
        return fail(st ? HPERR_NOPACK : HPERR_NETWORK);
    }
    t1 = GetTickCount();
    // How long each part took (what to make faster next).
    H64_INFO("[hirespack] downloaded %u KB in %.1f s (%.2f MB/s)", len >> 10, (t1 - t0) / 1000.0f,
             t1 > t0 ? len / 1048576.0f / ((t1 - t0) / 1000.0f) : 0.0f);
    {
        size_t ul = strlen(url);
        int zip = ul > 4 && !_stricmp(url + ul - 4, ".zip");
        InterlockedExchange(&s_state, zip ? HPACK_CONVERT : HPACK_UNPACK);
        st = zip ? png_convert_zip(archive, s_dest, s_file, sizeof(s_file)) : unpack(archive, s_dest, s_file, sizeof(s_file));
    }
    if (!st)
    {
        DeleteFileA(archive);
        return 0;
    }
    DeleteFileA(archive);
    H64_INFO("[hirespack] unpacked in %.1f s", (GetTickCount() - t1) / 1000.0f);
    install_name();
    H64_INFO("[hirespack] %s installed: %s\\%s", s_job.title, s_dest, s_file);
    InterlockedExchange(&s_state, HPACK_DONE);
    return 0;
}

void HiresPackStart(const HiresPackEntry *e, const char *destDir)
{
    HANDLE t;
    if (!e || HiresPackBusy()) return;
    s_job = *e;
    strncpy(s_dest, destDir, sizeof(s_dest) - 1);
    s_dest[sizeof(s_dest) - 1] = 0;
    s_file[0] = 0;
    s_dlNet.got = s_dlNet.total = 0;
    InterlockedExchange(&s_cancel, 0);
    InterlockedExchange(&s_error, HPERR_NONE);
    InterlockedExchange(&s_doneKb, 0);
    InterlockedExchange(&s_totalKb, 0);
    InterlockedExchange(&s_state, HPACK_LOOKUP);
    // A big stack for the TLS handshake, as the updater's; hardware thread 3.
    t = CreateThread(NULL, 256 * 1024, pack_thread, NULL, CREATE_SUSPENDED, NULL);
    if (!t) { fail(HPERR_NETWORK); return; }
    XSetThreadProcessor(t, 3);
    // Below the game's threads: it may go on while one plays (the in-game bar, main.cpp).
    SetThreadPriority(t, THREAD_PRIORITY_BELOW_NORMAL);
    ResumeThread(t);
    CloseHandle(t);
}

const HiresPackEntry *HiresPackJob(void)
{
    return s_state == HPACK_IDLE ? NULL : &s_job;
}

int HiresPackBusy(void)
{
    LONG st = s_state;
    return st == HPACK_LOOKUP || st == HPACK_DOWNLOAD || st == HPACK_UNPACK || st == HPACK_CONVERT;
}


// ---- The user's own pack (Settings > HD textures > Optimize my pack) ----
// A .zip (PNGs or a .hts), a .7z (a .hts), a .hts or a folder of PNGs the user copied into
// the import folder, made a DXT .hts in the folder of the game they chose, named after the
// file ("720.zip" -> "720.hts") and made that game's pack in use. The original stays.
static char s_src[256];
static int s_srcDir;

static DWORD WINAPI import_thread(LPVOID)
{
    const char *ext = strrchr(s_src, '.');
    DWORD t0 = GetTickCount();
    int ok;
    CreateDirectoryA(s_dest, NULL);
    if (s_srcDir) ok = png_convert_folder(s_src, s_dest, s_file, sizeof(s_file));
    else if (ext && !_stricmp(ext, ".zip")) ok = png_convert_zip(s_src, s_dest, s_file, sizeof(s_file));
    else if (ext && !_stricmp(ext, ".7z")) ok = unpack(s_src, s_dest, s_file, sizeof(s_file));
    else if (ext && !_stricmp(ext, ".hts")) ok = hts_from_file(s_src, s_dest, s_file, sizeof(s_file));
    else ok = fail(HPERR_ARCHIVE);
    if (!ok) return 0;
    install_name();
    H64_INFO("[hirespack] %s imported in %.1f s: %s\\%s", s_src, (GetTickCount() - t0) / 1000.0f, s_dest, s_file);
    InterlockedExchange(&s_state, HPACK_DONE);
    return 0;
}

int HiresImportKind(const char *name, int isDir)
{
    const char *ext = strrchr(name, '.');
    if (isDir) return HIMPORT_PNG;
    if (!ext) return 0;
    if (!_stricmp(ext, ".zip")) return HIMPORT_ZIP;
    if (!_stricmp(ext, ".7z")) return HIMPORT_7Z;
    if (!_stricmp(ext, ".hts")) return HIMPORT_HTS;
    return 0;
}

void HiresImportStart(const char *src, int isDir, const char *romName, const char *destDir)
{
    static char rom[64], title[64];
    static const HiresPackEntry job = { rom, "", "", title, "", NULL, 0, NULL };
    const char *base = src, *p, *dot;
    size_t n;
    HANDLE t;
    if (HiresPackBusy()) return;
    for (p = src; *p; p++)
        if (*p == '\\' || *p == '/') base = p + 1;
    dot = isDir ? NULL : strrchr(base, '.');
    n = dot ? (size_t)(dot - base) : strlen(base);
    if (n >= sizeof(title)) n = sizeof(title) - 1;
    memcpy(title, base, n);
    title[n] = 0;
    HiresFolderName(romName, rom, sizeof(rom));
    s_job = job;
    strncpy(s_src, src, sizeof(s_src) - 1);
    s_src[sizeof(s_src) - 1] = 0;
    s_srcDir = isDir;
    strncpy(s_dest, destDir, sizeof(s_dest) - 1);
    s_dest[sizeof(s_dest) - 1] = 0;
    s_file[0] = 0;
    InterlockedExchange(&s_cancel, 0);
    InterlockedExchange(&s_error, HPERR_NONE);
    InterlockedExchange(&s_doneKb, 0);
    InterlockedExchange(&s_totalKb, 0);
    InterlockedExchange(&s_state, HPACK_CONVERT);
    t = CreateThread(NULL, 256 * 1024, import_thread, NULL, CREATE_SUSPENDED, NULL);
    if (!t) { fail(HPERR_FILE); return; }
    XSetThreadProcessor(t, 3);
    SetThreadPriority(t, THREAD_PRIORITY_BELOW_NORMAL);
    ResumeThread(t);
    CloseHandle(t);
}

void HiresPngStart(const char *romName, const char *destDir)
{
    static char rom[64];
    static const HiresPackEntry png = { rom, "", "", "PNG", "", NULL, 0, NULL };
    HANDLE t;
    if (HiresPackBusy()) return;
    HiresFolderName(romName, rom, sizeof(rom));
    s_job = png;
    strncpy(s_dest, destDir, sizeof(s_dest) - 1);
    s_dest[sizeof(s_dest) - 1] = 0;
    s_file[0] = 0;
    InterlockedExchange(&s_cancel, 0);
    InterlockedExchange(&s_error, HPERR_NONE);
    InterlockedExchange(&s_doneKb, 0);
    InterlockedExchange(&s_totalKb, 0);
    InterlockedExchange(&s_state, HPACK_CONVERT);
    t = CreateThread(NULL, 256 * 1024, png_thread, NULL, CREATE_SUSPENDED, NULL);
    if (!t) { fail(HPERR_FILE); return; }
    XSetThreadProcessor(t, 3);
    SetThreadPriority(t, THREAD_PRIORITY_BELOW_NORMAL);   // as the downloads: one may play meanwhile
    ResumeThread(t);
    CloseHandle(t);
}

void HiresPackCancel(void)
{
    InterlockedExchange(&s_cancel, 1);
}

int HiresPackStatus(unsigned *doneKb, unsigned *totalKb, int *error, char *file, size_t fileSize)
{
    int st = (int)s_state;
    if (st == HPACK_DOWNLOAD)
    {
        *doneKb = (unsigned)((unsigned long)s_dlNet.got >> 10);
        *totalKb = (unsigned)((unsigned long)s_dlNet.total >> 10);
    }
    else
    {
        *doneKb = (unsigned)s_doneKb;
        *totalKb = (unsigned)s_totalKb;
    }
    if (error) *error = (int)s_error;
    if (file && fileSize)
    {
        strncpy(file, st == HPACK_DONE ? s_file : "", fileSize - 1);
        file[fileSize - 1] = 0;
    }
    return st;
}
