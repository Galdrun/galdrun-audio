"""Generate a short stereo tone for line-output testing; never play automatically."""
import argparse
import math
import struct
import wave

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("output")
parser.add_argument("--rate", type=int, choices=(44100, 48000), default=48000)
parser.add_argument("--db", type=int, choices=range(-60, -29), default=-60)
args = parser.parse_args()
frames = args.rate * 2
amplitude = 32767 * 10 ** (args.db / 20)
pcm = bytearray()
for index in range(frames):
    fade = min(1, index / (args.rate / 10), (frames - 1 - index) / (args.rate / 10))
    sample = round(amplitude * fade * math.sin(2 * math.pi * 440 * index / args.rate))
    pcm.extend(struct.pack("<hh", sample, sample))
assert len(pcm) == frames * 4
with wave.open(args.output, "wb") as output:
    output.setparams((2, 2, args.rate, frames, "NONE", "not compressed"))
    output.writeframes(pcm)
print(f"Created two-second 440 Hz stereo tone: {args.rate} Hz, {args.db} dBFS")
