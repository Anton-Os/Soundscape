# Soundscape

An audio library written in C, ported from the audio functionality of the Topl engine: playback and analysis of audio files, splitting a signal into frequency bands, and real-time capture from a microphone or the system output. Windows and Linux are the target platforms.

### Environment

External dependencies are fetched and built by CMake:

1. miniaudio for decoding, playback and capture
2. kissfft for the FFT behind the spectrum

MIDI support is optional and needs no dependency. Set `SUPPORT_MIDI` to include the plugin.

### Building

1. `cmake -S . -B build` (add `-DSUPPORT_MIDI=ON` for the MIDI plugin)
2. `cmake --build build`
3. `cmake --install build` installs the libraries, the headers and `SoundscapeConfig.cmake` into `build/INSTALL`

The first build needs a network connection to fetch miniaudio and kissfft.

### Project Structure

1. **core/** contains the library, with `Soundscape.h` as its only header
   - `Processing.c` decodes and plays audio files and measures signals (`SdSignal`, `SdTrack`)
   - `Banding.c` splits a signal into log spaced bands with levels, noise floors and beats (`SdBand`, `SdSpectrum`, `SdBands`)
   - `Streaming.c` captures a microphone or the system output in real time (`SdStream`)
2. **plugin/** contains the optional MIDI plugin, which reads and writes standard MIDI files (`SdMidi`)
3. **examples/** contains the Writer and Listener examples
4. **assets/** contains test tones from 20 Hz to 5000 Hz
5. **scripts/** contains build scripts used in the project

### Examples

**Writer** analyzes audio files and exports the band intensities as a table (`.csv`). With `SUPPORT_MIDI` it also exports the notes it hears as a MIDI file (`.mid`). Without arguments it exports the test tones.

    Writer [--output NAME] [--bands N] [--tempo BPM] [audio files]

**Listener** prints the band intensities of the audio it hears. With an audio file it plays the file and analyzes what is being heard, without one it captures the microphone.

    Listener [--offline] [--device N] [--devices] [--loopback] [--synth] [--bands N] [--goertzel] [--seconds S] [--interval S] [audio file]

Audio files are looked up as given first and then inside of the assets, so `Listener 300hz-sine-freqies.mp3` plays a test tone.
