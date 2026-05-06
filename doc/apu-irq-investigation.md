# APU IRQ wiring 調査メモ

セッション中断時のスナップショット (2026-05-06)。IRQ-driven music engine を持つ
小容量 ROM 群 (mapper 2/3、CHR-RAM 32-128 KB クラス) で音が出ない問題と、解消を
試みた経緯および挫折ポイントの記録。再開時はこのドキュメントから読み始めれば状況を
復元できる。

## 1. 何が起きているか

該当 ROM 群は boot 早期に `$4017=0x00` を書き込んで APU 4-step frame counter IRQ を
有効化し、IRQ ハンドラ (NES の `$FFFE` ベクタ) で music engine を駆動するクラシックな
実装。ところが本リポジトリの emulator では:

- **音が出ない**: ROM 自体は起動して画面も入力も動くが、APU register への書き込みが
  boot 直後に止まり、無音のままプレイ可能になる。
- **APU 書き込みカウンタ確認**: 該当 ROM 起動後 1 秒以降、`p1=0 p2=0 tri=0 nz=0
  w4015=0` がずっと続く。一方 IRQ-inhibit 型 (NMI-driven music の MMC3 RPG 等) では
  `p1≈200 p2≈200 ...` と active なまま。
- **PC range**: NMI handler と main loop は動いているが、IRQ-driven music routine
  には永遠に到達しない。

## 2. 真の原因

`apu_task` ([core/main/emu/emu_anemoia.cpp:191](../core/main/emu/emu_anemoia.cpp#L191))
が core 0 上で `Apu2A03::clock()` を busy-loop で呼び続けており、CPU の 6502 cycle と
**同期していない**。一方 CPU の `Bus::clock()` (フレーム単位) は core 1 上で別に走る。

実 HW では:

- `$4017` 書き込み直後に APU の frame-counter divider がリセットされる
- 次の IRQ assertion は 4-step / 5-step 1 周期後 (= 約 16ms 後) に発火
- ゲームはこの 16ms の間に IRQ ハンドラ context (zero page、stack、PRG-RAM) を準備可能

本実装では:

- apu_task は CPU の `$4017` write を観測した時点で既に独立に多数の APU clock を進めて
  いる
- 仮に `$4017` write 時に `clock_counter = 0; IRQ = false;` を実行しても、apu_task が
  すぐに `clock_counter` を 14914 まで進めて再 assertion してしまう
- ゲームが IRQ ハンドラを完全に準備する前に CPU が CLI したタイミングで `apu.IRQ` が
  既に立っているため、boot コードの中途半端な context で IRQ 処理に飛び、boot が破綻

## 3. これまでに試した方策 (すべて起動破壊で失敗)

### 3.1 単純配線

- `cpu6502.cpp::clock()` の命令境界で `apu.IRQ && (status & I) == 0` なら `IRQ()` を
  呼ぶ。
- 結果: boot 早期に IRQ が発火し、ゲーム本体が動かなくなる。

### 3.2 reset 時 I-flag = 1

- `Cpu6502::reset()` で `status = U | I` に変更 (実 HW 仕様準拠)。
- 結果: 単独で問題なし。3.1 と組み合わせても起動はハングしたまま。

### 3.3 `$4015` read を仕様準拠に

- 各チャネル length counter / DMC active / IRQ flag (bit 6) / DMC IRQ flag (bit 7) を
  正しく返す。
- 単独だと害は少ないが、3.1 と組み合わせても起動破壊。

### 3.4 `$4017` write で `clock_counter = 0; IRQ = false;`

- 実 HW の divider reset を模擬。
- 結果: 期待した 16ms grace は得られない。apu_task が core 0 で独立稼動なので、CPU が
  `$4017` を書いた直後に core 0 が `clock_counter` を即座にカウントし続ける。

### 3.5 reset 時 `interrupt_inhibit = true / clock_counter = 0` 既定値

- 起動直後は IRQ assertion を抑止。ゲームが `$4017` を明示的に書いた時点で初めて
  enable に切り替わる方針。
- IRQ-inhibit 型 (`$4017=0x40`) には影響なし、想定通り。
- IRQ-enable 型 (`$4017=0x00`) は明示的に IRQ enable する瞬間に上記 3.4 と同じ問題が
  発生。

## 4. 将来の修正方針候補

### 4.1 (本命) `apu_task` を廃止して APU を CPU cycle と同期

`Bus::clock()` の中で各 cpu.clock(N) 呼び出しの直後に APU を N (もしくは N/2) cycle だけ
進める形に変更。これにより:

- APU clock_counter は 6502 cycle と完全同期
- `$4017` write の divider リセットが直後の 16ms (= 14914 APU cycle) を正確に再現
- I2S への audio sample 出力は別経路で揃える必要あり (現状: apu_task の publish を
  emu_task で呼ぶ形に書き換え)

工数: 中〜大。`Apu2A03::clock` の呼び出し回数を CPU clock と紐付ける改造、I2S DMA への
push タイミング合わせが必要。

### 4.2 APU IRQ assertion に CPU cycle ベースの cooldown

`apu.IRQ` を assert する前に「直近の `$4017` write から N CPU cycle 経過しているか」を
チェックする gate を入れる。CPU cycle カウンタは emu_task 側で管理し、apu_task からは
別経路で同期。

工数: 小〜中だが、cross-core で CPU cycle カウンタを共有する設計が必要。

### 4.3 (応急処置) ROM 起動直後の N CPU cycle 間 IRQ delivery 抑止

CPU 側の IRQ poll に「reset 後 cumulative cycles >= 閾値」のガード追加。閾値は
2,000,000 cycles (≈ 1.1 秒) など。boot 完了まで IRQ 抑止することで、ゲームが grace
期間を確保できなくとも IRQ ハンドラ context が完成してから初回 IRQ 受け取りになる。

工数: 小。但しゲーム依存のマジック値が入るため hack 色が強い。

### 4.4 (代替アプローチ) `$4015` read で apu.IRQ を実機準拠に「前回 frame で立った IRQ flag」を返す

`apu.IRQ` の内部状態を「現在 asserted」と「前回 frame 終端 → CPU 観測予定」で 2 段に
分け、CPU 命令境界で polling する `apu.IRQ` は前段の遅延済みの方を見る。これにより
apu_task が最新 cycle で flag を立てても CPU 側は 1 frame 遅れで観測する形になる。

工数: 中。実 HW の挙動と微妙にずれるので互換性検証が必要。

## 5. 関連コード

| パス | 役割 |
|---|---|
| [core/main/emu/anemoia/apu2A03.cpp](../core/main/emu/anemoia/apu2A03.cpp) `Apu2A03::clock` | APU の 1 master cycle 進行。`clock_counter == 14914` で `IRQ = true` を立てる |
| [core/main/emu/anemoia/apu2A03.cpp](../core/main/emu/anemoia/apu2A03.cpp) `cpuWrite case 0x4017` | `$4017` write 処理。divider reset 未実装 |
| [core/main/emu/anemoia/cpu6502.cpp](../core/main/emu/anemoia/cpu6502.cpp) `Cpu6502::clock` | CPU instruction loop。今のところ `apu.IRQ` は見ていない |
| [core/main/emu/emu_anemoia.cpp](../core/main/emu/emu_anemoia.cpp) `apu_task_entry` | core 0 で `apu->clock()` を busy-loop |

## 6. 再開時の推奨ステップ

1. このドキュメントの "2. 真の原因" と "4. 将来の修正方針候補" を読む
2. **4.1 (apu_task 同期化) を最優先で検討**。これが正攻法でリスクが最も少ない
3. 既コミット済の `mapper001` / `ppu` diag (`grep -rn "diag (revertable)" core/main/`)
   は流用可能。APU 配線後の動作観察に活用
4. 動作確認は: NMI-driven music の MMC3 ROM で sound 継続、IRQ-driven music の
   小容量 ROM で sound が走り出す、他 ROM でリグレ無し、の 3 軸
