# MMC1 (mapper 1) post-config hang 調査メモ

セッション中断時のスナップショット (2026-05-04)。再開時はこのドキュメントから読み始めれば
状況を復元できる。

## 1. 何が起きているか

対象 ROM: mapper 1 (MMC1) / 256 KB PRG / CHR-RAM / battery-backed PRG-RAM 

ハング箇所: 主人公の名前入力 → 初期設定 (職業・性格選択等) を終えた直後、ゲーム本編が
始まる前のトランジションで固まる。プレイヤーはここから先に進めず、ゲーム本編をプレイ
できない状態。

ハング時の挙動 (実機 NTSC 出力):

- 画面は止まっているが描画は継続 (NMI が回り、PPU mask=0x18 で BG/sprite 描画は ON)
- フレームレートは ~50 fps (cf. NTSC target 60.098 fps、~80%)
- 入力には反応しない

## 2. これまでに確定した事実

### 2.1 mapper001 は無実

NMI ハンドラが毎フレーム書き込みを行う MMC1 sync は完璧に動作している。診断ログから
1 秒あたりの書き込み回数 (50 fps 状態):

| 種類 | 回数 / sec | 備考 |
|---|---|---|
| `INC $FFDF` (bit-7 reset) | 48 | 各 NMI 冒頭で実行 |
| 5-write to $9FFF (control reg) | 48 | ctrl=0x0E = mirror VERTICAL / PRG mode 3 / CHR mode 0 |
| 5-write to $BFFF (CHR bank 0) | 48 | 値 0x00 |
| 5-write to $DFFF (CHR bank 1) | 48 | 値 0x00 |
| 5-write to $FFFF (PRG bank) | 96 (×2) | 1 度目は bank 1 (NMI 内 sub のため)、戻しが bank 7 |

最終状態: `prg_mode=3 prg_bank=0x07` 安定。bank 7 が $8000-$BFFF、last bank が
$C000-$FFFF にマップ、これは正しい。

### 2.2 PPU vblank も正しく動作している

`[ppu-diag]` ログ:

```
setvbl=48 clrvbl=48 nmi=48 r2002_v0=188105 r2002_v1=48
```

- `setvbl/clrvbl/nmi` 各 48/sec → 1 frame ごとに 1 回だけ vblank flag が 0→1→0 し、
  NMI も 1 frame ごとに 1 回発火 (期待通り)
- $2002 read: vblank=0 を 188105 回、vblank=1 を 48 回 / sec

### 2.3 CPU は last bank ($C000-$FFFF) の wait loop を回り続けている

PC range 固定: `0xC444..0xC9AD` (= last bank のみ)

PC = `0xC444` または `0xC447` を交互サンプル。これは last bank 内の PPUMASK 更新ヘルパ
ルーチンの中の vblank 待ちループ:

```
$C441: AD 02 20    LDA $2002      ; PPUSTATUS read #1
$C444: AD 02 20    LDA $2002      ; PPUSTATUS read #2
$C447: 10 FB       BPL $C444      ; if bit 7 (vblank) clear, loop
$C449: 98          TYA            ; vblank set: continue
$C44A: 8D 01 20    STA $2001      ; write PPUMASK from Y
$C44D: 8D D4 06    STA $06D4      ; shadow copy
$C450: 60          RTS
```

呼び出し側は「PPUMASK を frame 安全に書き換える」目的で `LDY #VAL : JSR $C441` する。

### 2.4 wait loop は実は毎フレーム exit している

数値検算:

- visible 期間: cpu cycles ~ 27500 / frame
- ループ 1 周 = LDA (4 cy) + BPL taken (3 cy) = 7 cycles
- 27500 / 7 ≈ 3928 周 / frame
- 48 frames × 3928 = 188544 ≈ 実測 188105 → ピッタリ
- vblank=1 の read = 48 / sec = NMI 復帰直後の $C444 が 1 度だけ vblank=1 を観測 → BPL
  fall-through で exit、main 側が次のフレーム頭にまた `JSR $C441` を呼ぶ

つまり "wait loop で固着" ではなく、メインループは正常に進行しているが、
**$8000-$BFFF (swappable bank 7) の game-logic ルーチンを呼ばないコードパス** に
入ったまま last bank 内で PPUMASK 更新だけを繰り返している状態。

### 2.5 入力 poll が完全に止まっている

`strobes=0 reads=0` (1 秒間 $4016 への書き込み・読み出し共にゼロ)。
通常のゲームループは毎フレーム入力を poll するので、この状態は明らかに「入力を受け付ける
コードに入っていない」ことを示す。

## 3. これまでに当てた修正 (HW spec 準拠、保持すべき)

### 3.1 `mapper001_apply_banks` ヘルパー導入

`core/main/emu/anemoia/mappers/mapper001.cpp` に追加。control register ($8000-$9FFF
への 5-write 完了) の path で、mode 切替時に PRG/CHR の window pointer を即時再導出。

mapper004 で先行して入れていた "Re-apply MMC3 bank window on $8000 mode change"
(commit 34d982b) と同じ思想。

### 3.2 bit-7 reset path で派生 mode 再導出 + apply_banks 呼び出し

`mapper001.cpp` で bit 7 セット書き込み時、`state->control |= 0x0C` だけでなく
`PRG_ROM_bank_mode` / `CHR_ROM_bank_mode` を control から再導出し、bank pointer を
更新。これも HW 仕様準拠。

これらの 2 つは ROM-A の boot/プレイは通すものの、本ハングは解消しなかった。ただし
HW 仕様に対する明白な抜け穴を埋めているので、コミット候補として保持すべき変更。
revertable マーカーは付けていない (= 普通の修正コミット扱い)。

## 4. 入っている診断計装 (revertable、調査終了時に削除)

すべて `// BEGIN: ... diag (revertable)` ～ `// END: ... diag (revertable)` で囲って
あるので、`grep -rn "diag (revertable)"` で位置特定可能。

### 4.1 mapper001 diag

`core/main/emu/anemoia/mappers/mapper001.cpp`:

- 各 register write 完了カウンタ (w8000 / wA000 / wC000 / wE000 / bit7 / loads)
- 直近の制御値スナップショット (control / prg_mode / chr_mode / prg_bank / chr0 / chr1)

`core/main/main.cpp`:

- 上記 extern 宣言 + 1 Hz emu_task ログでの drain と `[mapper001-diag]` 行出力

### 4.2 ppu vblank diag

`core/main/emu/anemoia/ppu2C02.cpp`:

- setVBlank / clearVBlank 呼び出し回数
- NMI 発火回数
- PPUSTATUS ($2002) read 回数 (vblank=0 / vblank=1 で別カウント)

`core/main/main.cpp`:

- 上記 extern 宣言 + 1 Hz `[ppu-diag]` 行出力

### 4.3 削除手順

```sh
# 該当箇所の確認
grep -rn "diag (revertable)" core/main/

# 手で BEGIN/END ブロックごと削除、または:
# git reset HEAD~ で diag commit を破棄 (もし別 commit にしてあれば)
```

## 5. 何を疑うべきか (再開時の起点)

mapper001 / PPU vblank は無実が確定したので、ハングの原因は **「DQ 系 RPG の post-
config 状態が、何らかの "次へ進む条件" を待っているが、それが我々の emu では成立して
いない」** こと。具体的な候補:

### 5.1 APU 演奏完了待ち (本命)

post-config 直後に短いジングル / SE が鳴り、それが完了したら次画面へ遷移、という
シーケンスは国産 RPG では極めて典型的。

メインループは APU の channel disable 状態 / length counter / sweep status 等を
ポーリングしている可能性が高い。Anemoia の APU 実装で対象 channel が「演奏終了」を
正しく報告しているか要確認。

### 5.2 Sprite 0 hit 待ち

我々の PPU は `renderScanline` (line 396 ~) と `fakeSpriteHit` (line 481) で
sprite_zero_hit を立てる。FRAMESKIP で 1 frame おきに `fakeSpriteHit` 経路に切り替わ
るが、これが fall-through で hit を立てないケースが残っているかもしれない。
特に `mask.render_sprite` が 0 のときは即 return する (line 443) ので、ここで sprite
zero を期待しているコードがあれば永久に待つ。

### 5.3 別の RAM フラグ

NMI ハンドラ ($C955) には条件分岐が多数あり、`$32 == $FE` や `$1B bit 4 / bit 2` 等
で異なる経路に入る。post-config 状態ではこれらの flag が特定値になっていて、game-logic
に必要な path に入っていない可能性。

このパスを追うには、CPU 側に「現在の zero page $32 の値」「`$1B` の値」を 1 Hz でログに
出す instrumentation を追加すれば一気に見える。

### 5.4 その他薄め

- PRG-RAM の保存値検証ループ: 我々の PRG-RAM は単純な malloc なので失敗する理由は
  乏しい。
- DMC channel: APU の DMC は CPU clock を steal する。長時間 stall すれば NMI の
  cycle 予算をはみ出す。

## 6. 再開時の推奨ステップ

1. このドキュメントの "5. 何を疑うべきか" を読む
2. 5.1 (APU 完了) を最優先で検証する
   - Anemoia APU の各 channel の length counter / linear counter / DMC active
     状態を 1 Hz でログ出力する diag を追加 (revertable で)
   - post-config 状態で「どの channel が無限に鳴り続けているか」を観察
   - 該当 channel の length counter が我々の emu で減らない or 0 にならない
     条件を実装的に追う
3. APU が無罪と判明したら 5.3 (zp $32, $1B 等) の RAM トレース
4. それでも特定できなければ Anemoia upstream と本リポジトリの差分を全洗い

## 7. 関連コミット

```
34d982b Re-apply MMC3 bank window on $8000 mode change
66dc133 Skip the $2002 VBlank read-clear; sample 6502 PC
```

## 8. 参考: ハング時 1 Hz ログ典型例

```
[emu] frames=48 avg_update=19497us
[emu-diag] render_clocks=24 skip_clocks=24 publishes=720 render_frames=24
[emu-diag] mask_or=0x18 ctrl_or=0x90 strobes=0 reads=0 pad_or=0x00 strobe_or=0x00
[cpu-diag] pc=0xC447 range=0xC444..0xC9AD
[mapper001-diag] w8000=48 wA000=48 wC000=48 wE000=96 bit7=48 loads=1200
                 ctrl=0x0E prg_mode=3 chr_mode=0 prg_bank=0x07 chr0=0x00 chr1=0x00
[ppu-diag] setvbl=48 clrvbl=48 nmi=48 r2002_v0=188106 r2002_v1=48
```
