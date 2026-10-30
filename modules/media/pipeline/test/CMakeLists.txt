# Integration contract across input, converter, codec and publisher modules.
media_test(test_ffmpeg_timestamp_handling Media::Encoder Media::Decoder Media::Input Media::Publisher)

media_test(test_ffmpeg_audio_encoder Media::Encoder)
