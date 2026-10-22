# Server-side per-client audio streams

The architecture assumes that the audio server creates and manages a **separate logical audio stream for every connected client**.

There is one shared media timeline per zone, expressed in **nanoseconds**
(MATP section 5):

```text
Zone Media Timeline (nanoseconds)
        |
        +----> Client A Stream
        |
        +----> Client B Stream
        |
        +----> Client C Stream
```

Each client stream is independent regarding:

```text
network connection
stream format (sample rate, channels, sample layout)
buffer state
current output position
sound-card drift
playback offset
resampling ratio, where there is one at all
reconnect state
underrun state
```

The musical content and the timeline are shared, but the actual PCM stream sent to each client differs — in its format as well as in its length.

Example, one 44.1 kHz source and three rooms:

```text
Decoded source: 44,100 Hz / 2ch / 24-bit
      |
      +--------------------+--------------------+
      |                    |                    |
      v                    v                    v
Client A             Resampler Client B   Resampler Client C
no conversion        44.1 -> 48 kHz       44.1 -> 48 kHz
bitperfect           ratio 1.000000       ratio 1.000021
      |                    |                    |
      v                    v                    v
TCP Audio Stream A   TCP Audio Stream B   TCP Audio Stream C
44,100 Hz            48,000 Hz            48,000 Hz
      |                    |                    |
      v                    v                    v
Client A             Client B             Client C
```

Client A's device accepts the source's own rate, so the server sends the
decoded audio untouched: that room is **bitperfect** (MATP section 2). Client B
cannot do 44.1 kHz, so the server converts for that room alone. Client C is
also being drift-corrected, which is resampling by definition — so it cannot
be bitperfect, and that is a per-client decision the server owns (MATP section
28).

Therefore the server MUST NOT broadcast one identical PCM byte stream to every client.

Instead:

```text
one shared media timeline in nanoseconds
+
one individual output stream per client, in that client's own format
```

must be used.

Each connected client receives its own continuous stream.

The stream is started/prepared individually, but all clients are aligned to the same timeline and the same scheduled playback time.

For example:

```text
Client A:
stream prepared, 44,100 Hz
buffer filled
START_AT timelineNs 180,000,000,000 at server time X

Client B:
stream prepared, 48,000 Hz
buffer filled
START_AT timelineNs 180,000,000,000 at server time X

Client C:
stream prepared, 48,000 Hz
buffer filled
START_AT timelineNs 180,000,000,000 at server time X
```

All three make the same **media position** audible at the same target time. It
is a different frame of each room's own stream — frame 7,938,000 for A,
8,640,000 for B, and something a few frames either side of that for C — which
is why the protocol names positions in nanoseconds and never in frames (MATP
section 33).

After playback has started, the server continuously adjusts each client's output stream independently based on:

```text
measured hardware clock drift
playback offset
buffer state
```

Typical server-side state:

```cpp
struct ClientAudioStream
{
    ClientId clientId;

    AudioFormat streamFormat; // this client's own rate and layout
    bool bitperfect;          // false as soon as anything is converted

    uint64_t timelineNs;        // where this stream has got to on the timeline
    uint64_t outputFrame;       // and where that is on this client's own axis

    double measuredDriftPpm;
    double playbackOffsetMs;

    double resampleRatio;     // exactly 1 while the client is bitperfect

    BufferState bufferState;

    ConnectionState connectionState;
};
```

The two position fields belong together and must not be collapsed into one:
the first is comparable between rooms and the second is not. Keeping both is
what lets the server name the `timelineNs` of a scheduled event and know which
frame of this client's stream it will land on.

The exact implementation may differ.

The important architectural rule is:

> The media timeline is global, but the transported audio stream — its format as much as its length — is per client.

Do not couple the playback state of one client directly to the network/audio output state of another client.

A slow, reconnecting or drifting client must not disturb the streams of the other clients.

A client whose device has to be reopened for a source format change must not
either: MATP section 24 bounds how long the server waits for it, and the rooms
that did not have to reconfigure never leave the timeline.
