# NAM to CLO v2.7.0

Windows x64 application for converting Neural Amp Modeler (`.nam`) models to CLO files and uploading CLO files directly to Valeton GP-200 and GP-5 devices over USB MIDI.

The application has three tabs:

- **Convert to CLO** — convert one NAM model or every NAM model in a folder.
- **GP-200 Uploader** — upload an existing `.clo` file to one of the 10 GP-200 SnapTone slots.
- **GP-5 Uploader** — adapt a compatible CLO to the GP-5 transfer format and upload it to SnapTone 1-80.

> This is an independent research/reimplementation project and is not affiliated with or endorsed by Valeton or Hotone.

## Quick start

1. Build or download `NamToClo.exe`.
2. Keep `nam_input_wav.wav` in the **same folder as `NamToClo.exe`**.
3. Open **Convert to CLO**.
4. Select a `.nam` file or a folder containing `.nam` files.
5. Select the output folder.
6. Leave the optional processing disabled for a standard conversion, or configure **Tail / Reamp**, **Corrective IR** and/or **Tone Match** as described below.
7. Press **Convert**.

The repository includes the required `nam_input_wav.wav`. The supplied file is a 70-second, 44.1 kHz, 16-bit PCM WAV. The application adapts it internally to the mono stimulus used by the converter.

---

## Convert to CLO

![Convert to CLO tab](assets/convert-to-clo.jpg)

### Input NAM or folder

Use one of the two buttons:

- **Load NAM...** converts a single `.nam` model.
- **Load Folder...** batch-converts every `.nam` file found directly in the selected folder.

The selected path is shown in **Input NAM or folder**.

### Output folder

Choose where the generated CLO file or files will be written. **Open output folder** opens this location in Windows Explorer after conversion.

### Standard output

With **Tone Match disabled**, each NAM produces one GP-200 1024-tap CLO:

```text
<name>_NATIVE_GP200_1024.clo
```

No 2048-tap intermediate CLO is exported.

When converting a folder, the same settings are applied to every NAM in that folder.

---

## Required stimulus: `nam_input_wav.wav`

`nam_input_wav.wav` must be placed beside the executable:

```text
NamToClo.exe
nam_input_wav.wav
```

The converter always uses the original conversion stimulus. There is no stimulus-profile selector in the release version.

The stimulus contains two logical sections:

- **first 50 seconds** — fixed conversion stimulus;
- **final 20 seconds** — Tail / Reamp section.

The first 50 seconds always come from `nam_input_wav.wav`. The final 20 seconds are controlled by **Tail / Reamp source**.

---

## Tail / Reamp source

### Original Preset Audio

This is the default mode. The complete 70-second `nam_input_wav.wav` is used:

```text
0–50 s   original conversion stimulus
50–70 s  original preset/reamp tail
```

Use this for the normal conversion behavior.

### Recorded Audio

Choose **Recorded Audio** when you want to replace only the final 20-second Tail / Reamp section with your own WAV.

```text
0–50 s   original conversion stimulus
50–70 s  selected Recorded WAV
```

Press **Browse WAV...** and select the recording. The application adapts the selected file automatically:

- stereo or multichannel audio is downmixed to mono;
- common PCM and floating-point WAV formats are accepted;
- other sample rates are converted to 44.1 kHz;
- audio longer than 20 seconds is trimmed;
- audio shorter than 20 seconds is zero-padded.

The source file itself is not modified.

---

## Corrective IR

Enable **Apply corrective IR** to apply a corrective impulse-response WAV to the normal CLO result.

1. Tick **Apply corrective IR**.
2. Press **Browse WAV...**.
3. Select the corrective IR.
4. Convert normally.

The corrective stage is applied after the native NAM-to-CLO conversion and before the final GP-200 1024-tap CLO is written.

The generated filename remains:

```text
<name>_NATIVE_GP200_1024.clo
```

### Corrective IR and Tone Match together

When both options are enabled, the normal native conversion is completed first. The selected Corrective IR is then applied to the native CLO, and the same effective Corrective IR (including the CLO-side RMS normalization and post gain) is applied to the NAM render used as the Tone Match target. Tone Match therefore compares **NAM + Corrective IR** against **CLO + Corrective IR**, and refines the already-corrected CLO.

Using only Corrective IR or only Tone Match keeps the same behavior as before.

---

## Tone Match

Enable **Apply Tone Match (slow)** when you want the converter to perform the additional CLO refinement stage.

Tone Match is intentionally slower than the standard conversion.

When enabled, the normal CLO is not exported. The only result is:

```text
<name>_NATIVE_GP200_1024_TONEMATCH.clo
```

### Tone Match without a reference WAV

Leave **Tone Match reference WAV** empty. The converter uses the original conversion stimulus rendered through the NAM as the Tone Match target.

### Tone Match with a reference WAV

Use **Browse WAV...** under **Tone Match reference WAV** to select a different audio reference.

Only the **first 20 seconds** of the selected reference are used. The converter combines that reference with the fixed 50-second stimulus and renders the same test through the NAM before refining the CLO.

The reference WAV is adapted automatically in the same way as Recorded Audio: channel conversion, sample-rate adaptation, trimming and padding are handled by the application.

---

## Conversion status

The status bar at the bottom of the application reports the current operation. During NAM rendering and CLO fitting, the controls are temporarily disabled to prevent a second conversion from starting at the same time.

A successful single conversion produces exactly one `.clo` file. In batch mode, one final `.clo` is produced for each successfully converted NAM.

---

## GP-200 Uploader

![GP-200 Uploader tab](assets/gp200-uploader.jpg)

The uploader sends an existing GP-200 compatible `.clo` file directly to a SnapTone slot through USB MIDI.

### 1. Connect the GP-200

Connect the GP-200 to the computer through USB and power it on before uploading.

Open the **GP-200 Uploader** tab. The application scans the available MIDI input and output ports and shows the detected device under **USB MIDI device**.

If the pedal was connected after opening the application, press **Rescan**.

### 2. Select the CLO

Press **Browse CLO...** and select the `.clo` file to upload. You can also drag a `.clo` file onto the application window while the Uploader tab is selected.

### 3. Select the destination SnapTone slot

Choose one of the ten GP-200 SnapTone destinations:

```text
SnapTone 1  (AMP 1)
SnapTone 2  (AMP 2)
SnapTone 3  (AMP 3)
SnapTone 4  (AMP 4)
SnapTone 5  (AMP 5)
SnapTone 6  (DIST 1)
SnapTone 7  (DIST 2)
SnapTone 8  (DIST 3)
SnapTone 9  (DIST 4)
SnapTone 10 (DIST 5)
```

Uploading replaces the CLO currently stored in the selected destination slot.

### 4. Upload

Press **Upload to GP-200**. The transfer progress bar and bottom status line show the current transfer state. Do not disconnect or power off the GP-200 while the upload is in progress.

---

## GP-5 Uploader

The GP-5 uploader implements the USB-MIDI transfer reconstructed from Valeton Suite captures. It accepts compatible VTSI/HTSI CLO files containing **A=128** and at least **512 B taps**. This includes the 1024-tap CLO files produced by the current converter and 2048-tap Valeton/Hotone CLO files.

### 1. Connect the GP-5

Connect and power on the GP-5, open the **GP-5 Uploader** tab, and press **Rescan** if the MIDI device was connected after the application started.

### 2. Select the CLO

Press **Browse CLO...** or drag a `.clo` file onto the application while the GP-5 tab is active. The source file is not modified.

Before transfer, the uploader creates the GP-5 runtime representation in memory:

```text
VTSI
A = 128 taps
B = first 512 taps
declared size = 0x0A88
payload size  = 0x0A00
```

The internal CLO CRC16/MODBUS is recalculated automatically.

### 3. Select SnapTone 1-80

Choose the destination slot. Valeton Suite uses zero-based slot numbering internally, so visible SnapTone 1 maps to slot byte `0x00` and visible SnapTone 80 maps to `0x4F`.

### 4. Upload

Press **Upload to GP-5**. The uploader wraps the compact CLO in the reconstructed 74-byte GP-5 SnapTone header, splits the 2770-byte transfer into 146 command-`0x92` blocks, calculates the packet CRC8, nibble-encodes each SysEx message, and waits for the GP-5 ACK before advancing to the next block.

The progress bar reaches 146 blocks and the application then waits for the final GP-5 completion message. A block is retried automatically if its ACK is not received within the timeout.

> The GP-5 upload implementation is based on USB-MIDI captures of Valeton Suite uploads to SnapTone 51 and SnapTone 80. It intentionally implements only the transfer required for SnapTone upload; it does not reproduce the full Valeton Suite startup/state synchronization.

---

## File layout for a release

A minimal usable folder is:

```text
NamToClo.exe
nam_input_wav.wav
```

No external runtime folder is required for the Convert to CLO tab.

---

## Build from source

Requirements:

- Windows x64
- CMake 3.24 or newer
- Visual Studio / MSVC with C++ desktop development tools

Build with:

```powershell
cmake --preset windows-x64
cmake --build build --config Release --parallel
```

The executable is generated by the CMake build in the Release output directory.

---

## Licensing

See [`LICENSE`](LICENSE) for the project license and [`THIRD_PARTY.md`](THIRD_PARTY.md) for third-party components and their licenses.
