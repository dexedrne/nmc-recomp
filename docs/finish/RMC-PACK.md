# Roxas May Cry package 1

Use the full `pack/` tree over the installed RMC 0.1.0 data. Build the lifted host with [runtime.patch](runtime.patch) for the castle rules, one-HP finish and Skip on phone and PC. The pack marker selects the RMC defaults at startup. A data-only install on an older executable supplies the models, rewards, rooms and report images. For the Steam native DLL, [native.patch](native.patch) includes the corresponding gameplay settings in the RMC profile.

## Switches

| Restoration | How to turn it off |
|---|---|
| Castle encounter rules | Set `KHNMC_PS2_CASTLE=0` in the lifted host environment. The one-HP setting inherits this switch unless set explicitly. On Steam set `ps2_castle=off` under `[gameplay]`. |
| Story and Data Final Xemnas one-HP finish | Set `KHNMC_PS2_FINAL_ONE_HP=0` or native `ps2_final_one_hp=off`. The resistance rules still follow the castle switch. |
| Ending Skip | Set `KHNMC_RMC_ENDING_PAUSE=0` or native `ending_pause=off`. |
| Pre-castle Awakened reward | Rebuild with `--no-reward`. F12's Bond of Flame and eligibility flag and F14's elevator payout stay available. |
| Xemnas I appearance | Rebuild with `--no-skin`. |
| Shortened scene 995 | Rebuild with `--no-short-ending`. The original long scene can be skipped with the independent Skip switch. |

Run `python3 -B build_pack.py OUTPUT` here with a fresh directory name. The three data switches can be combined. Select that complete directory as the overlay so disabled assets from a prior installation cannot take precedence. The Linux null replay uses an absolute `KHNMC_RMC_DIR` because the route runner changes its working directory. Port users can also set any runtime switch to 1. The Steam settings are independent.

## Composition

The builder checks the 26 inherited files against [base.json](base.json). The owner's November 20 decision selects F22's story /74 b_40 and Data /98 b_80 in eh20, and F21's /73 partner repair in eh23. The historical [stack preflight](run-P1/eh20.log) records both alternatives. R20 supplies the eight PC-named ending images.

In tt40, the shared /1 entry fix and the existing /161 reward survive. Comparing the shared room against the pinned F12 output also found the missing /2 Progress instruction for flag 18498. The builder restores it and adds list 622 after 317 in /161. It reuses F12's event result and keeps other BAR members in place. In 03system the installed PS01 table is the ancestor. F14 appends the kind-1 treasure 245, Betwixt adds key 622, and the Xemnas I recipe changes the separate memt member. The builder checks the treasure append and every final member payload.

## File inventory

264 files. SHA-256 identifies the exact local output.

| File | Recipe or inherited release | SHA-256 |
|---|---|---|
| `00objentry.bin` | xemnas-I.json | `291a1b2941d026b326f1c231be45662a04f8c4d3176879cbc7f4d54532147575` |
| `00progress.bin` | F8 Neverland final release selected by F15 | `db3926e478602d382bb9576339783e320c85fe5ef9f99e2a85f830aa519d6cfc` |
| `03system.bin` | Installed PS01 + F14 ascent payout + PNACH Betwixt treasure + Xemnas I memt | `cbc8381ce1b5ed5419aaf7f35671bddbbd543881e1570ea1b570d07105ae69ca` |
| `ard/us/al05.ard` | F6-lexaeus | `943100fca23357e26eb045f9528fc0257534412ea16148ce20fcf20069ed77bd` |
| `ard/us/eh03.ard` | F13-neoshadow | `615e71fe2b609e01e1588f2dfb2f50b2fffaaab8efe640002054f9ece5ef73b8` |
| `ard/us/eh07.ard` | F14-ascent | `d9a6ee80e826b2e3a42b74dbf6bf7555e96ef03a2033f9ea2294a1305316e974` |
| `ard/us/eh20.ard` | F22-final | `398503304cfa6ed683582bd8d3638d27f8176f77b5bafb7dd28fc550bb090038` |
| `ard/us/eh22.ard` | F20-dragon | `7d45ef33726b2aae14c0a02a20edf7ff850c7ea382fcfd5c4effe6d532470ced` |
| `ard/us/eh23.ard` | F21 Armored II | `2b13dc4c406dc18209ad328dcbd5f757f0867d2961e29aa062da440bdbfff6af` |
| `ard/us/es00.ard` | ending-995.json | `be650154d76d9fdc2896b9f52546a70e4742a5349f1984bc32cbafc44bcc5966` |
| `ard/us/lk03.ard` | F8-neverland | `05f0316a935245cc44772a7273def003c05a6ba230aaa3c3a89d23365139cdb3` |
| `ard/us/lk05.ard` | F8-neverland | `a59a636bf6a17af37704f8094da312214d9b3a4b58d857c78e7402e62084a5ea` |
| `ard/us/po00.ard` | F10-day96 | `8f365fdd012728f519995f3c48638491084b8e83a24c3e98562fd30226c9d7e1` |
| `ard/us/po07.ard` | F10-day96 | `bd6c1d9dc5ebae6bbe2a7f2142ba11d156ec5705d0e5567f39031173c4fb4f0f` |
| `ard/us/tt40.ard` | F12 Betwixt /1, /2, /161 and PNACH Betwixt reward | `15b0a125219e3b54d309ef175b45537dfa90cab84b88a53ca043b5392efdf788` |
| `itempic/item-360.imd` | betwixt.json · PS2 reward icon | `f175dfca08cc539fbf44736474eb75a2367ad36b75c225876626208cdc672a1a` |
| `libretto-lm.bar` | F9-traverse | `1abbfc8deb2d0307a4b91ecc5559e216fcc7291fa42a8b312ddf560c2005d45f` |
| `map/al05.map` | F6-lexaeus | `d2080e286173891cf05e40e2c1586c4a2b4a23cf6b8baa683f3e2e62a7f92396` |
| `map/eh22.map` | F20-dragon | `c05d0e9e117df4fdb742c5d000aeefb2738c23ec0a1e352c9e869d3ef85f9108` |
| `msg/us/nm.bar` | F7-larxene | `51b1e62d53b2065785657cb2de50337448cbd436872e66a4f3cc552151fcf0d2` |
| `msn/us/NM11_FM_LAR.bar` | F7-larxene | `4cefcb23baf98be79b1c817e6bf2588921f3f013d7118f645a8f6524fc02af06` |
| `obj/N_CM000_BTL.mdlx` | F5-doom | `6b4c61540a74d1eecc9092537fa7c381c7954e9b6d71b2e256edce886f554e5f` |
| `obj/P_EX102.a.us` | xemnas-I.json | `078d6ffb200adbdda221b26d67eea5a348334b00c1bd0651ec2f1d10751a7cbd` |
| `obj/P_EX102.mdlx` | xemnas-I.json | `39bfcf6e438980cea05da7a53716738135bbdd817926990325a20d2411a7f926` |
| `obj/P_EX102.mset` | xemnas-I.json | `1527945691fcd9f713a1f246a2fcf6760a5b311dbd1cf18d242473b9de2bf1d4` |
| `obj/P_EX102_BTLF.a.us` | xemnas-I.json | `ba58eda57744951caf8f0073d09567de9b05ed590ac6d4f1b6ced7ff474e592a` |
| `obj/P_EX102_BTLF.mdlx` | xemnas-I.json | `8b5100a6199ecc93a70b0fba3cbcc99d3fdbe1e837edb18df89f804ae20011db` |
| `obj/P_EX102_BTLF.mset` | xemnas-I.json | `dfcb2e944f71d4c0177e532f5c970568e688f9fc2435b3a3bd1867f693a2d9a2` |
| `obj/P_EX102_KH1F.a.us` | xemnas-I.json | `ba58eda57744951caf8f0073d09567de9b05ed590ac6d4f1b6ced7ff474e592a` |
| `obj/P_EX102_KH1F.mdlx` | xemnas-I.json | `8b5100a6199ecc93a70b0fba3cbcc99d3fdbe1e837edb18df89f804ae20011db` |
| `obj/P_EX102_KH1F.mset` | xemnas-I.json | `dfcb2e944f71d4c0177e532f5c970568e688f9fc2435b3a3bd1867f693a2d9a2` |
| `obj/P_EX102_MAGF.a.us` | xemnas-I.json | `6d74994205db95b981593f4c9e9a3d9865f75150fe7b68b75dcdcad54695d64e` |
| `obj/P_EX102_MAGF.mdlx` | xemnas-I.json | `780819f5865068916c821c0139c912dfea3c66170f8395ccd02c8a5ce546bddf` |
| `obj/P_EX102_MAGF.mset` | xemnas-I.json | `ee1e3499ea101f99ff972a62faee919837d010e579a527a1c748be8e14499df7` |
| `obj/P_EX102_ULTF.a.us` | xemnas-I.json | `f85c9960fc506e00635de7a763907851bb12bc7aa84040510582132a2a2b3f5c` |
| `obj/P_EX102_ULTF.mdlx` | xemnas-I.json | `700d9d5073e6275a6f885b3282fb285d50ab899f91043713b3da9e3f1ca7b2e5` |
| `obj/P_EX102_ULTF.mset` | xemnas-I.json | `452391cd8c19647a158b312d4d1493432bfef7709266f9460de2de2663c4c053` |
| `remastered/menu/us/ending.2ld/US_ending_2ld0.png` | R20 ending report | `991b01c3cb3737d66e2e63b1b533dd1c3c0a7c07718594c4ebe7ac8d12b6bf86` |
| `remastered/menu/us/ending.2ld/US_ending_2ld1.png` | R20 ending report | `345764f7f79ab9b22a2cf8aaa15a0a8a9c23acfcc3b97f8b9ab8d7df4f38f75c` |
| `remastered/menu/us/ending.2ld/US_ending_2ld2.png` | R20 ending report | `2ba8a120d3f3d1589115c2f57567070116873bf273a58e3f29b28e53aad79702` |
| `remastered/menu/us/ending.2ld/US_ending_2ld3.png` | R20 ending report | `8a4884ab422bdfdc8ab6dd5acec853c1be0133aba55d38bc788d962906fd0e17` |
| `remastered/menu/us/ending.2ld/US_ending_2ld4.png` | R20 ending report | `b1f8a95d3750cf6f1fd5e3c5edb4502f76ffa1223720e2f316e7c61c295e43f5` |
| `remastered/menu/us/ending.2ld/US_ending_2ld5.png` | R20 ending report | `a7e9f8d67968a1b354e763f0d7f287b6f249707ccfc886a62d504b7cb223a992` |
| `remastered/menu/us/ending.2ld/US_ending_2ld6.png` | R20 ending report | `80353b7afa5832d4614e25bdbd1fee253e0e4cdd9140cc10b2e5f67e10013415` |
| `remastered/menu/us/ending.2ld/US_ending_2ld7.png` | R20 ending report | `ff961caa39b8f7770b8469d71b741281a3c612aef4764dd42af1be6c92d6122e` |
| `remastered/obj/P_EX102.a.us/-0.dds` | xemnas-I.json | `ae97506f517cc4bb35f0c34b12a487df752680dbb34ab37d1a642cfe4d77c9fa` |
| `remastered/obj/P_EX102.a.us/-1.dds` | xemnas-I.json | `239b17d51b20bbad76a9619a5e2e7ff4df5a781fe2d18d6b4a66a3be6c28d852` |
| `remastered/obj/P_EX102.a.us/-10.dds` | xemnas-I.json | `4469f68e43cfb68d492d78ab28fed2343062f9ed72c0d413bea6caf1b4227d44` |
| `remastered/obj/P_EX102.a.us/-11.dds` | xemnas-I.json | `5ed3d87b1ba01d1cda0a55f397a13a9939279296c40f446ceaaab25ffebe2edd` |
| `remastered/obj/P_EX102.a.us/-12.dds` | xemnas-I.json | `8a9a176450acc564a5b5ae804f3a020f8c7155ca974902e9606b0a02a1469a62` |
| `remastered/obj/P_EX102.a.us/-13.dds` | xemnas-I.json | `c7d3e794b35e4c10c490a053a3cdbe92371ece474c1399edc72b7a471b9f7ee4` |
| `remastered/obj/P_EX102.a.us/-14.dds` | xemnas-I.json | `5b1aa56aab17b3d9b18e26f6082fe4e33b91a04815bc343b763d343a3c751ad9` |
| `remastered/obj/P_EX102.a.us/-15.dds` | xemnas-I.json | `94db8c8d8f9c279842e49a2c88d8a1a55fba1b27b86e014a4a9947fd051c1b26` |
| `remastered/obj/P_EX102.a.us/-16.dds` | xemnas-I.json | `3e7861ef8a143336213030b4101344f3d86c40d50851ab936a0494501177cb5a` |
| `remastered/obj/P_EX102.a.us/-17.dds` | xemnas-I.json | `3db8ff413de521c16484f455d9d22d4f8aa0a435d5db1c14129477ddf36e4802` |
| `remastered/obj/P_EX102.a.us/-18.dds` | xemnas-I.json | `d21a3e4543e14bc61e19dcf6de954adc7791b31f99769ec3c0189084675682d0` |
| `remastered/obj/P_EX102.a.us/-19.dds` | xemnas-I.json | `0f30a511d578aa85c8d1f536187cbe26ae02aa6a28b63439c73a27c3ee6a91af` |
| `remastered/obj/P_EX102.a.us/-2.dds` | xemnas-I.json | `e5b5b71b991aa2008cb599674367dbe6b8ad84558faf71dcf013a561fc9fc8ca` |
| `remastered/obj/P_EX102.a.us/-20.dds` | xemnas-I.json | `a9ef3d9dbae712e64c2f07d24f91b3b20ff7066dd31fee695b6e8bf89e4e96bf` |
| `remastered/obj/P_EX102.a.us/-21.dds` | xemnas-I.json | `d18a2993136d2f74eff157b03df803f42bd376f92e082707fe54369279807450` |
| `remastered/obj/P_EX102.a.us/-22.dds` | xemnas-I.json | `4cd5675ce20b5cd296967f94ddd30fd4bdf4171bca9badbd09694ff068fc9855` |
| `remastered/obj/P_EX102.a.us/-23.dds` | xemnas-I.json | `8986d526de516068f7fc67aa20b6105bc1a4c0a11e35422c50ba07e947839af2` |
| `remastered/obj/P_EX102.a.us/-24.dds` | xemnas-I.json | `aa0161d9aa0573b96815984d6f0e66b19b0530398ff0f89f12b85d4dbae80946` |
| `remastered/obj/P_EX102.a.us/-25.dds` | xemnas-I.json | `114fa48153997e4c7d6e500ea2b05bfc67c0eb93abfc471705a3b33d9e2a1cb6` |
| `remastered/obj/P_EX102.a.us/-26.dds` | xemnas-I.json | `ed098f663bd844a18a25f7c234d61d49db6e3a18a0df8e7ef9d3a5bab5401a1b` |
| `remastered/obj/P_EX102.a.us/-27.dds` | xemnas-I.json | `e366974d2238b457c67002841f6c75ce81a9aaee8efafd5c818830e206fcc200` |
| `remastered/obj/P_EX102.a.us/-28.dds` | xemnas-I.json | `76e6bb5d66e026a99e167ae12d8047020f1354f01d9bf23b0de94feed38a2d1c` |
| `remastered/obj/P_EX102.a.us/-29.dds` | xemnas-I.json | `c48c196f593169ba8947f6b5fab5e3d40cd658e50f5379cef6bfe1c60fd6c7b5` |
| `remastered/obj/P_EX102.a.us/-3.dds` | xemnas-I.json | `639fd5f4c13c952c590402d7cd25234b8f80da0699913299cd08d3ca9ad203f4` |
| `remastered/obj/P_EX102.a.us/-30.dds` | xemnas-I.json | `dfe589462a58dd2bef7873def38c7a56465ae2357e12ec9349d5c99daf9055b2` |
| `remastered/obj/P_EX102.a.us/-31.dds` | xemnas-I.json | `0bf74de895fce309b77d3af6e884789371622de6aa425135a31be7f6291144f1` |
| `remastered/obj/P_EX102.a.us/-32.dds` | xemnas-I.json | `bca54bf27965b61520e4640d012e26030b4498a6dbb19a332529fbad2a2bea0b` |
| `remastered/obj/P_EX102.a.us/-33.dds` | xemnas-I.json | `886be5525ae15c5dc919da85dcb898886a62fd43cb4fdb9fc06c46ce6c343232` |
| `remastered/obj/P_EX102.a.us/-34.dds` | xemnas-I.json | `a717fef985804a932a94df55a413ba3402593ecfba32d3c853b390b3f36b1c3c` |
| `remastered/obj/P_EX102.a.us/-35.dds` | xemnas-I.json | `88d43d41131532cc6b0a2bb8d67dc8b6c5a23fc92e3fb8450b0c5614da190d97` |
| `remastered/obj/P_EX102.a.us/-36.dds` | xemnas-I.json | `a23cf78b20b3bf89fb5bf79f989d1bf837d3598fbc1543e03a34c7ef0cd47fc1` |
| `remastered/obj/P_EX102.a.us/-4.dds` | xemnas-I.json | `8e0e0124155733853f09869c61dc91534f85b48cdafc4a365fb8ba94426f0ad1` |
| `remastered/obj/P_EX102.a.us/-5.dds` | xemnas-I.json | `6d5af5bc3fc9da01fc523ac4ad95f66b7c03b14650515418caef27bf45a85759` |
| `remastered/obj/P_EX102.a.us/-6.dds` | xemnas-I.json | `6d643c619a797be8a9be0303111fa64746c547751956760ba923470cb7323fc4` |
| `remastered/obj/P_EX102.a.us/-7.dds` | xemnas-I.json | `3fda558b7f62a994873efdcef4e646a2827789247ca2e4be33ec2492cee6485e` |
| `remastered/obj/P_EX102.a.us/-8.dds` | xemnas-I.json | `3f11744d1b14e733e6e39fbcc428d37828b72b6c284aae92253334c51234b0c1` |
| `remastered/obj/P_EX102.a.us/-9.dds` | xemnas-I.json | `da4071e5bd991dd894f60a688affb5acefd4ebe1047c550c9aabe79cb328e628` |
| `remastered/obj/P_EX102_BTLF.a.us/-0.dds` | xemnas-I.json | `108f710a1272363e72169d43a997ad2e81339455cd89737bab3984cf89fae553` |
| `remastered/obj/P_EX102_BTLF.a.us/-1.dds` | xemnas-I.json | `ae97506f517cc4bb35f0c34b12a487df752680dbb34ab37d1a642cfe4d77c9fa` |
| `remastered/obj/P_EX102_BTLF.a.us/-10.dds` | xemnas-I.json | `da4071e5bd991dd894f60a688affb5acefd4ebe1047c550c9aabe79cb328e628` |
| `remastered/obj/P_EX102_BTLF.a.us/-11.dds` | xemnas-I.json | `4469f68e43cfb68d492d78ab28fed2343062f9ed72c0d413bea6caf1b4227d44` |
| `remastered/obj/P_EX102_BTLF.a.us/-12.dds` | xemnas-I.json | `5ed3d87b1ba01d1cda0a55f397a13a9939279296c40f446ceaaab25ffebe2edd` |
| `remastered/obj/P_EX102_BTLF.a.us/-13.dds` | xemnas-I.json | `8a9a176450acc564a5b5ae804f3a020f8c7155ca974902e9606b0a02a1469a62` |
| `remastered/obj/P_EX102_BTLF.a.us/-14.dds` | xemnas-I.json | `c7d3e794b35e4c10c490a053a3cdbe92371ece474c1399edc72b7a471b9f7ee4` |
| `remastered/obj/P_EX102_BTLF.a.us/-15.dds` | xemnas-I.json | `5b1aa56aab17b3d9b18e26f6082fe4e33b91a04815bc343b763d343a3c751ad9` |
| `remastered/obj/P_EX102_BTLF.a.us/-16.dds` | xemnas-I.json | `94db8c8d8f9c279842e49a2c88d8a1a55fba1b27b86e014a4a9947fd051c1b26` |
| `remastered/obj/P_EX102_BTLF.a.us/-17.dds` | xemnas-I.json | `3e7861ef8a143336213030b4101344f3d86c40d50851ab936a0494501177cb5a` |
| `remastered/obj/P_EX102_BTLF.a.us/-18.dds` | xemnas-I.json | `3db8ff413de521c16484f455d9d22d4f8aa0a435d5db1c14129477ddf36e4802` |
| `remastered/obj/P_EX102_BTLF.a.us/-19.dds` | xemnas-I.json | `d21a3e4543e14bc61e19dcf6de954adc7791b31f99769ec3c0189084675682d0` |
| `remastered/obj/P_EX102_BTLF.a.us/-2.dds` | xemnas-I.json | `239b17d51b20bbad76a9619a5e2e7ff4df5a781fe2d18d6b4a66a3be6c28d852` |
| `remastered/obj/P_EX102_BTLF.a.us/-20.dds` | xemnas-I.json | `0f30a511d578aa85c8d1f536187cbe26ae02aa6a28b63439c73a27c3ee6a91af` |
| `remastered/obj/P_EX102_BTLF.a.us/-21.dds` | xemnas-I.json | `a9ef3d9dbae712e64c2f07d24f91b3b20ff7066dd31fee695b6e8bf89e4e96bf` |
| `remastered/obj/P_EX102_BTLF.a.us/-22.dds` | xemnas-I.json | `d18a2993136d2f74eff157b03df803f42bd376f92e082707fe54369279807450` |
| `remastered/obj/P_EX102_BTLF.a.us/-23.dds` | xemnas-I.json | `4cd5675ce20b5cd296967f94ddd30fd4bdf4171bca9badbd09694ff068fc9855` |
| `remastered/obj/P_EX102_BTLF.a.us/-24.dds` | xemnas-I.json | `8986d526de516068f7fc67aa20b6105bc1a4c0a11e35422c50ba07e947839af2` |
| `remastered/obj/P_EX102_BTLF.a.us/-25.dds` | xemnas-I.json | `f79c8b3f6b4c3ab66ab4e8357b2ac15e0558aa23eeb5449cd041560db0128a93` |
| `remastered/obj/P_EX102_BTLF.a.us/-26.dds` | xemnas-I.json | `d21a3e4543e14bc61e19dcf6de954adc7791b31f99769ec3c0189084675682d0` |
| `remastered/obj/P_EX102_BTLF.a.us/-27.dds` | xemnas-I.json | `efe2bb645149045e84b481aa5c6b68fe01ec6092f998c907d3ead5d545a750f1` |
| `remastered/obj/P_EX102_BTLF.a.us/-28.dds` | xemnas-I.json | `1368d026dd6d7b90cb13b8c5252e9cf9d849a60ccc2533c65697261420c737eb` |
| `remastered/obj/P_EX102_BTLF.a.us/-29.dds` | xemnas-I.json | `4cd5675ce20b5cd296967f94ddd30fd4bdf4171bca9badbd09694ff068fc9855` |
| `remastered/obj/P_EX102_BTLF.a.us/-3.dds` | xemnas-I.json | `e5b5b71b991aa2008cb599674367dbe6b8ad84558faf71dcf013a561fc9fc8ca` |
| `remastered/obj/P_EX102_BTLF.a.us/-30.dds` | xemnas-I.json | `2f99086bb3949c7db89a487b64ef71810dd0a8c349c1fce3e06f0026c7572582` |
| `remastered/obj/P_EX102_BTLF.a.us/-31.dds` | xemnas-I.json | `f6b11880ae0b25944e9c07f4e85b2d5d4564a618c91a166f92c2ed6ef90627b7` |
| `remastered/obj/P_EX102_BTLF.a.us/-32.dds` | xemnas-I.json | `f3f2607f2a403deb172d20991ab57527f2374f6588a962136cf7ec09e5a15a6e` |
| `remastered/obj/P_EX102_BTLF.a.us/-33.dds` | xemnas-I.json | `56adcc2addfce7a6a1b8565ab38ca761f02127e7ab3879cd64549f2fae7cfa0f` |
| `remastered/obj/P_EX102_BTLF.a.us/-34.dds` | xemnas-I.json | `a14ffbd083ce114d36a1b22f042ac957aa7d6a94097776f92b7c39eb09e740a9` |
| `remastered/obj/P_EX102_BTLF.a.us/-35.dds` | xemnas-I.json | `a5709e0258efbfe0b4b016e24d300257873cfd9581fe7a7c9d00b6ef4609190b` |
| `remastered/obj/P_EX102_BTLF.a.us/-36.dds` | xemnas-I.json | `ed098f663bd844a18a25f7c234d61d49db6e3a18a0df8e7ef9d3a5bab5401a1b` |
| `remastered/obj/P_EX102_BTLF.a.us/-37.dds` | xemnas-I.json | `e366974d2238b457c67002841f6c75ce81a9aaee8efafd5c818830e206fcc200` |
| `remastered/obj/P_EX102_BTLF.a.us/-38.dds` | xemnas-I.json | `76e6bb5d66e026a99e167ae12d8047020f1354f01d9bf23b0de94feed38a2d1c` |
| `remastered/obj/P_EX102_BTLF.a.us/-39.dds` | xemnas-I.json | `bca54bf27965b61520e4640d012e26030b4498a6dbb19a332529fbad2a2bea0b` |
| `remastered/obj/P_EX102_BTLF.a.us/-4.dds` | xemnas-I.json | `639fd5f4c13c952c590402d7cd25234b8f80da0699913299cd08d3ca9ad203f4` |
| `remastered/obj/P_EX102_BTLF.a.us/-40.dds` | xemnas-I.json | `886be5525ae15c5dc919da85dcb898886a62fd43cb4fdb9fc06c46ce6c343232` |
| `remastered/obj/P_EX102_BTLF.a.us/-41.dds` | xemnas-I.json | `b8a22fa16d91686935cf1406b01fecae3d1dba9e9de874dd2cf96220c6f4b167` |
| `remastered/obj/P_EX102_BTLF.a.us/-42.dds` | xemnas-I.json | `46985b070d909a2cf6a003192ba89004c0cc8825ea1b346bd2df8f644a6d87f6` |
| `remastered/obj/P_EX102_BTLF.a.us/-43.dds` | xemnas-I.json | `3e1e3561466ee3053c131e3003ef44532b36d971c285df6077bd3adc032884a3` |
| `remastered/obj/P_EX102_BTLF.a.us/-44.dds` | xemnas-I.json | `d18a2993136d2f74eff157b03df803f42bd376f92e082707fe54369279807450` |
| `remastered/obj/P_EX102_BTLF.a.us/-45.dds` | xemnas-I.json | `083459f0c3d7d5588178018e5c92db5c3e64835dedcded8929f4cb5df353ee5e` |
| `remastered/obj/P_EX102_BTLF.a.us/-46.dds` | xemnas-I.json | `b8a22fa16d91686935cf1406b01fecae3d1dba9e9de874dd2cf96220c6f4b167` |
| `remastered/obj/P_EX102_BTLF.a.us/-47.dds` | xemnas-I.json | `b39af1ac348848b811e94de0c038ef59f1fef0e514eaa73486d7c853bf042bf1` |
| `remastered/obj/P_EX102_BTLF.a.us/-5.dds` | xemnas-I.json | `8e0e0124155733853f09869c61dc91534f85b48cdafc4a365fb8ba94426f0ad1` |
| `remastered/obj/P_EX102_BTLF.a.us/-6.dds` | xemnas-I.json | `6d5af5bc3fc9da01fc523ac4ad95f66b7c03b14650515418caef27bf45a85759` |
| `remastered/obj/P_EX102_BTLF.a.us/-7.dds` | xemnas-I.json | `6d643c619a797be8a9be0303111fa64746c547751956760ba923470cb7323fc4` |
| `remastered/obj/P_EX102_BTLF.a.us/-8.dds` | xemnas-I.json | `3fda558b7f62a994873efdcef4e646a2827789247ca2e4be33ec2492cee6485e` |
| `remastered/obj/P_EX102_BTLF.a.us/-9.dds` | xemnas-I.json | `3f11744d1b14e733e6e39fbcc428d37828b72b6c284aae92253334c51234b0c1` |
| `remastered/obj/P_EX102_BTLF.a.us/voice/battle/us/lnd` | xemnas-I.json | `f0669f30ee6c0f1c3850c30bd8e44e95f5fc9268739d660d752ff8f033a06d39` |
| `remastered/obj/P_EX102_KH1F.a.us/-0.dds` | xemnas-I.json | `108f710a1272363e72169d43a997ad2e81339455cd89737bab3984cf89fae553` |
| `remastered/obj/P_EX102_KH1F.a.us/-1.dds` | xemnas-I.json | `ae97506f517cc4bb35f0c34b12a487df752680dbb34ab37d1a642cfe4d77c9fa` |
| `remastered/obj/P_EX102_KH1F.a.us/-10.dds` | xemnas-I.json | `da4071e5bd991dd894f60a688affb5acefd4ebe1047c550c9aabe79cb328e628` |
| `remastered/obj/P_EX102_KH1F.a.us/-11.dds` | xemnas-I.json | `4469f68e43cfb68d492d78ab28fed2343062f9ed72c0d413bea6caf1b4227d44` |
| `remastered/obj/P_EX102_KH1F.a.us/-12.dds` | xemnas-I.json | `5ed3d87b1ba01d1cda0a55f397a13a9939279296c40f446ceaaab25ffebe2edd` |
| `remastered/obj/P_EX102_KH1F.a.us/-13.dds` | xemnas-I.json | `8a9a176450acc564a5b5ae804f3a020f8c7155ca974902e9606b0a02a1469a62` |
| `remastered/obj/P_EX102_KH1F.a.us/-14.dds` | xemnas-I.json | `c7d3e794b35e4c10c490a053a3cdbe92371ece474c1399edc72b7a471b9f7ee4` |
| `remastered/obj/P_EX102_KH1F.a.us/-15.dds` | xemnas-I.json | `5b1aa56aab17b3d9b18e26f6082fe4e33b91a04815bc343b763d343a3c751ad9` |
| `remastered/obj/P_EX102_KH1F.a.us/-16.dds` | xemnas-I.json | `94db8c8d8f9c279842e49a2c88d8a1a55fba1b27b86e014a4a9947fd051c1b26` |
| `remastered/obj/P_EX102_KH1F.a.us/-17.dds` | xemnas-I.json | `3e7861ef8a143336213030b4101344f3d86c40d50851ab936a0494501177cb5a` |
| `remastered/obj/P_EX102_KH1F.a.us/-18.dds` | xemnas-I.json | `3db8ff413de521c16484f455d9d22d4f8aa0a435d5db1c14129477ddf36e4802` |
| `remastered/obj/P_EX102_KH1F.a.us/-19.dds` | xemnas-I.json | `d21a3e4543e14bc61e19dcf6de954adc7791b31f99769ec3c0189084675682d0` |
| `remastered/obj/P_EX102_KH1F.a.us/-2.dds` | xemnas-I.json | `239b17d51b20bbad76a9619a5e2e7ff4df5a781fe2d18d6b4a66a3be6c28d852` |
| `remastered/obj/P_EX102_KH1F.a.us/-20.dds` | xemnas-I.json | `0f30a511d578aa85c8d1f536187cbe26ae02aa6a28b63439c73a27c3ee6a91af` |
| `remastered/obj/P_EX102_KH1F.a.us/-21.dds` | xemnas-I.json | `a9ef3d9dbae712e64c2f07d24f91b3b20ff7066dd31fee695b6e8bf89e4e96bf` |
| `remastered/obj/P_EX102_KH1F.a.us/-22.dds` | xemnas-I.json | `d18a2993136d2f74eff157b03df803f42bd376f92e082707fe54369279807450` |
| `remastered/obj/P_EX102_KH1F.a.us/-23.dds` | xemnas-I.json | `4cd5675ce20b5cd296967f94ddd30fd4bdf4171bca9badbd09694ff068fc9855` |
| `remastered/obj/P_EX102_KH1F.a.us/-24.dds` | xemnas-I.json | `8986d526de516068f7fc67aa20b6105bc1a4c0a11e35422c50ba07e947839af2` |
| `remastered/obj/P_EX102_KH1F.a.us/-25.dds` | xemnas-I.json | `f79c8b3f6b4c3ab66ab4e8357b2ac15e0558aa23eeb5449cd041560db0128a93` |
| `remastered/obj/P_EX102_KH1F.a.us/-26.dds` | xemnas-I.json | `d21a3e4543e14bc61e19dcf6de954adc7791b31f99769ec3c0189084675682d0` |
| `remastered/obj/P_EX102_KH1F.a.us/-27.dds` | xemnas-I.json | `efe2bb645149045e84b481aa5c6b68fe01ec6092f998c907d3ead5d545a750f1` |
| `remastered/obj/P_EX102_KH1F.a.us/-28.dds` | xemnas-I.json | `1368d026dd6d7b90cb13b8c5252e9cf9d849a60ccc2533c65697261420c737eb` |
| `remastered/obj/P_EX102_KH1F.a.us/-29.dds` | xemnas-I.json | `4cd5675ce20b5cd296967f94ddd30fd4bdf4171bca9badbd09694ff068fc9855` |
| `remastered/obj/P_EX102_KH1F.a.us/-3.dds` | xemnas-I.json | `e5b5b71b991aa2008cb599674367dbe6b8ad84558faf71dcf013a561fc9fc8ca` |
| `remastered/obj/P_EX102_KH1F.a.us/-30.dds` | xemnas-I.json | `2f99086bb3949c7db89a487b64ef71810dd0a8c349c1fce3e06f0026c7572582` |
| `remastered/obj/P_EX102_KH1F.a.us/-31.dds` | xemnas-I.json | `f6b11880ae0b25944e9c07f4e85b2d5d4564a618c91a166f92c2ed6ef90627b7` |
| `remastered/obj/P_EX102_KH1F.a.us/-32.dds` | xemnas-I.json | `f3f2607f2a403deb172d20991ab57527f2374f6588a962136cf7ec09e5a15a6e` |
| `remastered/obj/P_EX102_KH1F.a.us/-33.dds` | xemnas-I.json | `56adcc2addfce7a6a1b8565ab38ca761f02127e7ab3879cd64549f2fae7cfa0f` |
| `remastered/obj/P_EX102_KH1F.a.us/-34.dds` | xemnas-I.json | `a14ffbd083ce114d36a1b22f042ac957aa7d6a94097776f92b7c39eb09e740a9` |
| `remastered/obj/P_EX102_KH1F.a.us/-35.dds` | xemnas-I.json | `a5709e0258efbfe0b4b016e24d300257873cfd9581fe7a7c9d00b6ef4609190b` |
| `remastered/obj/P_EX102_KH1F.a.us/-36.dds` | xemnas-I.json | `ed098f663bd844a18a25f7c234d61d49db6e3a18a0df8e7ef9d3a5bab5401a1b` |
| `remastered/obj/P_EX102_KH1F.a.us/-37.dds` | xemnas-I.json | `e366974d2238b457c67002841f6c75ce81a9aaee8efafd5c818830e206fcc200` |
| `remastered/obj/P_EX102_KH1F.a.us/-38.dds` | xemnas-I.json | `76e6bb5d66e026a99e167ae12d8047020f1354f01d9bf23b0de94feed38a2d1c` |
| `remastered/obj/P_EX102_KH1F.a.us/-39.dds` | xemnas-I.json | `bca54bf27965b61520e4640d012e26030b4498a6dbb19a332529fbad2a2bea0b` |
| `remastered/obj/P_EX102_KH1F.a.us/-4.dds` | xemnas-I.json | `639fd5f4c13c952c590402d7cd25234b8f80da0699913299cd08d3ca9ad203f4` |
| `remastered/obj/P_EX102_KH1F.a.us/-40.dds` | xemnas-I.json | `886be5525ae15c5dc919da85dcb898886a62fd43cb4fdb9fc06c46ce6c343232` |
| `remastered/obj/P_EX102_KH1F.a.us/-41.dds` | xemnas-I.json | `b8a22fa16d91686935cf1406b01fecae3d1dba9e9de874dd2cf96220c6f4b167` |
| `remastered/obj/P_EX102_KH1F.a.us/-42.dds` | xemnas-I.json | `46985b070d909a2cf6a003192ba89004c0cc8825ea1b346bd2df8f644a6d87f6` |
| `remastered/obj/P_EX102_KH1F.a.us/-43.dds` | xemnas-I.json | `3e1e3561466ee3053c131e3003ef44532b36d971c285df6077bd3adc032884a3` |
| `remastered/obj/P_EX102_KH1F.a.us/-44.dds` | xemnas-I.json | `d18a2993136d2f74eff157b03df803f42bd376f92e082707fe54369279807450` |
| `remastered/obj/P_EX102_KH1F.a.us/-45.dds` | xemnas-I.json | `083459f0c3d7d5588178018e5c92db5c3e64835dedcded8929f4cb5df353ee5e` |
| `remastered/obj/P_EX102_KH1F.a.us/-46.dds` | xemnas-I.json | `b8a22fa16d91686935cf1406b01fecae3d1dba9e9de874dd2cf96220c6f4b167` |
| `remastered/obj/P_EX102_KH1F.a.us/-47.dds` | xemnas-I.json | `b39af1ac348848b811e94de0c038ef59f1fef0e514eaa73486d7c853bf042bf1` |
| `remastered/obj/P_EX102_KH1F.a.us/-5.dds` | xemnas-I.json | `8e0e0124155733853f09869c61dc91534f85b48cdafc4a365fb8ba94426f0ad1` |
| `remastered/obj/P_EX102_KH1F.a.us/-6.dds` | xemnas-I.json | `6d5af5bc3fc9da01fc523ac4ad95f66b7c03b14650515418caef27bf45a85759` |
| `remastered/obj/P_EX102_KH1F.a.us/-7.dds` | xemnas-I.json | `6d643c619a797be8a9be0303111fa64746c547751956760ba923470cb7323fc4` |
| `remastered/obj/P_EX102_KH1F.a.us/-8.dds` | xemnas-I.json | `3fda558b7f62a994873efdcef4e646a2827789247ca2e4be33ec2492cee6485e` |
| `remastered/obj/P_EX102_KH1F.a.us/-9.dds` | xemnas-I.json | `3f11744d1b14e733e6e39fbcc428d37828b72b6c284aae92253334c51234b0c1` |
| `remastered/obj/P_EX102_KH1F.a.us/voice/battle/us/lnd` | xemnas-I.json | `f0669f30ee6c0f1c3850c30bd8e44e95f5fc9268739d660d752ff8f033a06d39` |
| `remastered/obj/P_EX102_MAGF.a.us/-0.dds` | xemnas-I.json | `ae97506f517cc4bb35f0c34b12a487df752680dbb34ab37d1a642cfe4d77c9fa` |
| `remastered/obj/P_EX102_MAGF.a.us/-1.dds` | xemnas-I.json | `239b17d51b20bbad76a9619a5e2e7ff4df5a781fe2d18d6b4a66a3be6c28d852` |
| `remastered/obj/P_EX102_MAGF.a.us/-10.dds` | xemnas-I.json | `4469f68e43cfb68d492d78ab28fed2343062f9ed72c0d413bea6caf1b4227d44` |
| `remastered/obj/P_EX102_MAGF.a.us/-11.dds` | xemnas-I.json | `5ed3d87b1ba01d1cda0a55f397a13a9939279296c40f446ceaaab25ffebe2edd` |
| `remastered/obj/P_EX102_MAGF.a.us/-12.dds` | xemnas-I.json | `8a9a176450acc564a5b5ae804f3a020f8c7155ca974902e9606b0a02a1469a62` |
| `remastered/obj/P_EX102_MAGF.a.us/-13.dds` | xemnas-I.json | `c7d3e794b35e4c10c490a053a3cdbe92371ece474c1399edc72b7a471b9f7ee4` |
| `remastered/obj/P_EX102_MAGF.a.us/-14.dds` | xemnas-I.json | `5b1aa56aab17b3d9b18e26f6082fe4e33b91a04815bc343b763d343a3c751ad9` |
| `remastered/obj/P_EX102_MAGF.a.us/-15.dds` | xemnas-I.json | `94db8c8d8f9c279842e49a2c88d8a1a55fba1b27b86e014a4a9947fd051c1b26` |
| `remastered/obj/P_EX102_MAGF.a.us/-16.dds` | xemnas-I.json | `3e7861ef8a143336213030b4101344f3d86c40d50851ab936a0494501177cb5a` |
| `remastered/obj/P_EX102_MAGF.a.us/-17.dds` | xemnas-I.json | `3db8ff413de521c16484f455d9d22d4f8aa0a435d5db1c14129477ddf36e4802` |
| `remastered/obj/P_EX102_MAGF.a.us/-18.dds` | xemnas-I.json | `ab09671f2464c76051e03c1eac9a6afc47a842031de9ff1309093491bb403446` |
| `remastered/obj/P_EX102_MAGF.a.us/-19.dds` | xemnas-I.json | `10a4d06599f6369f48740d70edbb2dfc32aa1a07d92d90afd5a49ce45031a9b9` |
| `remastered/obj/P_EX102_MAGF.a.us/-2.dds` | xemnas-I.json | `e5b5b71b991aa2008cb599674367dbe6b8ad84558faf71dcf013a561fc9fc8ca` |
| `remastered/obj/P_EX102_MAGF.a.us/-20.dds` | xemnas-I.json | `d21a3e4543e14bc61e19dcf6de954adc7791b31f99769ec3c0189084675682d0` |
| `remastered/obj/P_EX102_MAGF.a.us/-21.dds` | xemnas-I.json | `0f30a511d578aa85c8d1f536187cbe26ae02aa6a28b63439c73a27c3ee6a91af` |
| `remastered/obj/P_EX102_MAGF.a.us/-22.dds` | xemnas-I.json | `4b67a12de1268ecbfef981c7cefdc53edd6a2a8bddfd64f85dc4d4da8b8d14ef` |
| `remastered/obj/P_EX102_MAGF.a.us/-23.dds` | xemnas-I.json | `c455e4ae4a40dc41e49b00a8ae3ac08aef5110ccb22922eeff8561a29fcd3428` |
| `remastered/obj/P_EX102_MAGF.a.us/-24.dds` | xemnas-I.json | `3a7c1502185ce8d7f886e1e1aff1a54ba482d1b76784eec834c1ce79ed10415b` |
| `remastered/obj/P_EX102_MAGF.a.us/-25.dds` | xemnas-I.json | `083459f0c3d7d5588178018e5c92db5c3e64835dedcded8929f4cb5df353ee5e` |
| `remastered/obj/P_EX102_MAGF.a.us/-26.dds` | xemnas-I.json | `0bf74de895fce309b77d3af6e884789371622de6aa425135a31be7f6291144f1` |
| `remastered/obj/P_EX102_MAGF.a.us/-27.dds` | xemnas-I.json | `4b67a12de1268ecbfef981c7cefdc53edd6a2a8bddfd64f85dc4d4da8b8d14ef` |
| `remastered/obj/P_EX102_MAGF.a.us/-28.dds` | xemnas-I.json | `ba602a15c12d57103be56652fe0f24e2f40fa794d4304333ba165e55c1875724` |
| `remastered/obj/P_EX102_MAGF.a.us/-29.dds` | xemnas-I.json | `95af1789df080ceb383d19bd50ff5a2d42975532d56b43f7b482473fcfe890ff` |
| `remastered/obj/P_EX102_MAGF.a.us/-3.dds` | xemnas-I.json | `639fd5f4c13c952c590402d7cd25234b8f80da0699913299cd08d3ca9ad203f4` |
| `remastered/obj/P_EX102_MAGF.a.us/-30.dds` | xemnas-I.json | `d43509bd5ff0e7b0aa1177867f18c2be342d7b7f9df36580b452285bdaaa43a7` |
| `remastered/obj/P_EX102_MAGF.a.us/-31.dds` | xemnas-I.json | `8a9a176450acc564a5b5ae804f3a020f8c7155ca974902e9606b0a02a1469a62` |
| `remastered/obj/P_EX102_MAGF.a.us/-32.dds` | xemnas-I.json | `9da3c637ae321b3ba45607a3d4c580f815d56e24860e8ea984fdb58851732d8b` |
| `remastered/obj/P_EX102_MAGF.a.us/-33.dds` | xemnas-I.json | `c612955cfc94c65345d3aee4d5d09f5c879a2237231b979e464311dcd76f47da` |
| `remastered/obj/P_EX102_MAGF.a.us/-34.dds` | xemnas-I.json | `50f35b921bde4bdb17c2c205ea0d65f14e3c36739f6b2a41daf0fc532e66e851` |
| `remastered/obj/P_EX102_MAGF.a.us/-35.dds` | xemnas-I.json | `ed098f663bd844a18a25f7c234d61d49db6e3a18a0df8e7ef9d3a5bab5401a1b` |
| `remastered/obj/P_EX102_MAGF.a.us/-36.dds` | xemnas-I.json | `e366974d2238b457c67002841f6c75ce81a9aaee8efafd5c818830e206fcc200` |
| `remastered/obj/P_EX102_MAGF.a.us/-37.dds` | xemnas-I.json | `76e6bb5d66e026a99e167ae12d8047020f1354f01d9bf23b0de94feed38a2d1c` |
| `remastered/obj/P_EX102_MAGF.a.us/-38.dds` | xemnas-I.json | `b39af1ac348848b811e94de0c038ef59f1fef0e514eaa73486d7c853bf042bf1` |
| `remastered/obj/P_EX102_MAGF.a.us/-4.dds` | xemnas-I.json | `8e0e0124155733853f09869c61dc91534f85b48cdafc4a365fb8ba94426f0ad1` |
| `remastered/obj/P_EX102_MAGF.a.us/-5.dds` | xemnas-I.json | `6d5af5bc3fc9da01fc523ac4ad95f66b7c03b14650515418caef27bf45a85759` |
| `remastered/obj/P_EX102_MAGF.a.us/-6.dds` | xemnas-I.json | `6d643c619a797be8a9be0303111fa64746c547751956760ba923470cb7323fc4` |
| `remastered/obj/P_EX102_MAGF.a.us/-7.dds` | xemnas-I.json | `3fda558b7f62a994873efdcef4e646a2827789247ca2e4be33ec2492cee6485e` |
| `remastered/obj/P_EX102_MAGF.a.us/-8.dds` | xemnas-I.json | `3f11744d1b14e733e6e39fbcc428d37828b72b6c284aae92253334c51234b0c1` |
| `remastered/obj/P_EX102_MAGF.a.us/-9.dds` | xemnas-I.json | `da4071e5bd991dd894f60a688affb5acefd4ebe1047c550c9aabe79cb328e628` |
| `remastered/obj/P_EX102_MAGF.a.us/voice/battle/us/bladeless` | xemnas-I.json | `ef2664056b3461f3a81eca886e4b16b71b97eba4fd67870e55d6b6d2d06d28cb` |
| `remastered/obj/P_EX102_ULTF.a.us/-0.dds` | xemnas-I.json | `ae97506f517cc4bb35f0c34b12a487df752680dbb34ab37d1a642cfe4d77c9fa` |
| `remastered/obj/P_EX102_ULTF.a.us/-1.dds` | xemnas-I.json | `239b17d51b20bbad76a9619a5e2e7ff4df5a781fe2d18d6b4a66a3be6c28d852` |
| `remastered/obj/P_EX102_ULTF.a.us/-10.dds` | xemnas-I.json | `4469f68e43cfb68d492d78ab28fed2343062f9ed72c0d413bea6caf1b4227d44` |
| `remastered/obj/P_EX102_ULTF.a.us/-11.dds` | xemnas-I.json | `5ed3d87b1ba01d1cda0a55f397a13a9939279296c40f446ceaaab25ffebe2edd` |
| `remastered/obj/P_EX102_ULTF.a.us/-12.dds` | xemnas-I.json | `8a9a176450acc564a5b5ae804f3a020f8c7155ca974902e9606b0a02a1469a62` |
| `remastered/obj/P_EX102_ULTF.a.us/-13.dds` | xemnas-I.json | `c7d3e794b35e4c10c490a053a3cdbe92371ece474c1399edc72b7a471b9f7ee4` |
| `remastered/obj/P_EX102_ULTF.a.us/-14.dds` | xemnas-I.json | `5b1aa56aab17b3d9b18e26f6082fe4e33b91a04815bc343b763d343a3c751ad9` |
| `remastered/obj/P_EX102_ULTF.a.us/-15.dds` | xemnas-I.json | `94db8c8d8f9c279842e49a2c88d8a1a55fba1b27b86e014a4a9947fd051c1b26` |
| `remastered/obj/P_EX102_ULTF.a.us/-16.dds` | xemnas-I.json | `3e7861ef8a143336213030b4101344f3d86c40d50851ab936a0494501177cb5a` |
| `remastered/obj/P_EX102_ULTF.a.us/-17.dds` | xemnas-I.json | `3db8ff413de521c16484f455d9d22d4f8aa0a435d5db1c14129477ddf36e4802` |
| `remastered/obj/P_EX102_ULTF.a.us/-18.dds` | xemnas-I.json | `d21a3e4543e14bc61e19dcf6de954adc7791b31f99769ec3c0189084675682d0` |
| `remastered/obj/P_EX102_ULTF.a.us/-19.dds` | xemnas-I.json | `0f30a511d578aa85c8d1f536187cbe26ae02aa6a28b63439c73a27c3ee6a91af` |
| `remastered/obj/P_EX102_ULTF.a.us/-2.dds` | xemnas-I.json | `e5b5b71b991aa2008cb599674367dbe6b8ad84558faf71dcf013a561fc9fc8ca` |
| `remastered/obj/P_EX102_ULTF.a.us/-20.dds` | xemnas-I.json | `a9ef3d9dbae712e64c2f07d24f91b3b20ff7066dd31fee695b6e8bf89e4e96bf` |
| `remastered/obj/P_EX102_ULTF.a.us/-21.dds` | xemnas-I.json | `d18a2993136d2f74eff157b03df803f42bd376f92e082707fe54369279807450` |
| `remastered/obj/P_EX102_ULTF.a.us/-22.dds` | xemnas-I.json | `4cd5675ce20b5cd296967f94ddd30fd4bdf4171bca9badbd09694ff068fc9855` |
| `remastered/obj/P_EX102_ULTF.a.us/-23.dds` | xemnas-I.json | `8986d526de516068f7fc67aa20b6105bc1a4c0a11e35422c50ba07e947839af2` |
| `remastered/obj/P_EX102_ULTF.a.us/-24.dds` | xemnas-I.json | `f79c8b3f6b4c3ab66ab4e8357b2ac15e0558aa23eeb5449cd041560db0128a93` |
| `remastered/obj/P_EX102_ULTF.a.us/-25.dds` | xemnas-I.json | `d21a3e4543e14bc61e19dcf6de954adc7791b31f99769ec3c0189084675682d0` |
| `remastered/obj/P_EX102_ULTF.a.us/-26.dds` | xemnas-I.json | `efe2bb645149045e84b481aa5c6b68fe01ec6092f998c907d3ead5d545a750f1` |
| `remastered/obj/P_EX102_ULTF.a.us/-27.dds` | xemnas-I.json | `1368d026dd6d7b90cb13b8c5252e9cf9d849a60ccc2533c65697261420c737eb` |
| `remastered/obj/P_EX102_ULTF.a.us/-28.dds` | xemnas-I.json | `4cd5675ce20b5cd296967f94ddd30fd4bdf4171bca9badbd09694ff068fc9855` |
| `remastered/obj/P_EX102_ULTF.a.us/-29.dds` | xemnas-I.json | `2f99086bb3949c7db89a487b64ef71810dd0a8c349c1fce3e06f0026c7572582` |
| `remastered/obj/P_EX102_ULTF.a.us/-3.dds` | xemnas-I.json | `639fd5f4c13c952c590402d7cd25234b8f80da0699913299cd08d3ca9ad203f4` |
| `remastered/obj/P_EX102_ULTF.a.us/-30.dds` | xemnas-I.json | `d21a3e4543e14bc61e19dcf6de954adc7791b31f99769ec3c0189084675682d0` |
| `remastered/obj/P_EX102_ULTF.a.us/-31.dds` | xemnas-I.json | `0f30a511d578aa85c8d1f536187cbe26ae02aa6a28b63439c73a27c3ee6a91af` |
| `remastered/obj/P_EX102_ULTF.a.us/-32.dds` | xemnas-I.json | `083459f0c3d7d5588178018e5c92db5c3e64835dedcded8929f4cb5df353ee5e` |
| `remastered/obj/P_EX102_ULTF.a.us/-33.dds` | xemnas-I.json | `6de95049fdc9f7aadd670fbf46b4281c434fc94260d6194bdc142018c0853d07` |
| `remastered/obj/P_EX102_ULTF.a.us/-34.dds` | xemnas-I.json | `ed098f663bd844a18a25f7c234d61d49db6e3a18a0df8e7ef9d3a5bab5401a1b` |
| `remastered/obj/P_EX102_ULTF.a.us/-35.dds` | xemnas-I.json | `e366974d2238b457c67002841f6c75ce81a9aaee8efafd5c818830e206fcc200` |
| `remastered/obj/P_EX102_ULTF.a.us/-36.dds` | xemnas-I.json | `76e6bb5d66e026a99e167ae12d8047020f1354f01d9bf23b0de94feed38a2d1c` |
| `remastered/obj/P_EX102_ULTF.a.us/-37.dds` | xemnas-I.json | `f3f2607f2a403deb172d20991ab57527f2374f6588a962136cf7ec09e5a15a6e` |
| `remastered/obj/P_EX102_ULTF.a.us/-38.dds` | xemnas-I.json | `56adcc2addfce7a6a1b8565ab38ca761f02127e7ab3879cd64549f2fae7cfa0f` |
| `remastered/obj/P_EX102_ULTF.a.us/-39.dds` | xemnas-I.json | `a14ffbd083ce114d36a1b22f042ac957aa7d6a94097776f92b7c39eb09e740a9` |
| `remastered/obj/P_EX102_ULTF.a.us/-4.dds` | xemnas-I.json | `8e0e0124155733853f09869c61dc91534f85b48cdafc4a365fb8ba94426f0ad1` |
| `remastered/obj/P_EX102_ULTF.a.us/-40.dds` | xemnas-I.json | `a5709e0258efbfe0b4b016e24d300257873cfd9581fe7a7c9d00b6ef4609190b` |
| `remastered/obj/P_EX102_ULTF.a.us/-41.dds` | xemnas-I.json | `a23cf78b20b3bf89fb5bf79f989d1bf837d3598fbc1543e03a34c7ef0cd47fc1` |
| `remastered/obj/P_EX102_ULTF.a.us/-5.dds` | xemnas-I.json | `6d5af5bc3fc9da01fc523ac4ad95f66b7c03b14650515418caef27bf45a85759` |
| `remastered/obj/P_EX102_ULTF.a.us/-6.dds` | xemnas-I.json | `6d643c619a797be8a9be0303111fa64746c547751956760ba923470cb7323fc4` |
| `remastered/obj/P_EX102_ULTF.a.us/-7.dds` | xemnas-I.json | `3fda558b7f62a994873efdcef4e646a2827789247ca2e4be33ec2492cee6485e` |
| `remastered/obj/P_EX102_ULTF.a.us/-8.dds` | xemnas-I.json | `3f11744d1b14e733e6e39fbcc428d37828b72b6c284aae92253334c51234b0c1` |
| `remastered/obj/P_EX102_ULTF.a.us/-9.dds` | xemnas-I.json | `da4071e5bd991dd894f60a688affb5acefd4ebe1047c550c9aabe79cb328e628` |
| `remastered/obj/P_EX102_ULTF.a.us/voice/battle/us/awakened` | xemnas-I.json | `3c368c71edb5adec4f719838f24e24b2992154b16e7b7d4d5e41836e77d1c70a` |
| `rmc-package-1` | Package 1 runtime defaults | `4355a46b19d348dc2f57c046f8ef63d4538ebb936000f3c9ee954a27460dd865` |
