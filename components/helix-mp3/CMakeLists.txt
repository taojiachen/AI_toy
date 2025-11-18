set(PUBLIC_REQUIREMENTS ${PUBLIC_REQUIREMENTS} "helix_mp3" CACHE STRING "public requirement for main" FORCE)
message("-- Build component : ${PUBLIC_REQUIREMENTS}")

list(APPEND ADD_INCLUDE
    "./fixpnt/pub"
    "./fixpnt/real"
)

list(APPEND ADD_SRCS
    "./fixpnt/mp3dec.c"
    "./fixpnt/mp3tabs.c"
    "./fixpnt/real/bitstream.c"
    "./fixpnt/real/buffers.c"
    "./fixpnt/real/dct32.c"
    "./fixpnt/real/dequant.c"
    "./fixpnt/real/dqchan.c"
    "./fixpnt/real/huffman.c"
    "./fixpnt/real/hufftabs.c"
    "./fixpnt/real/imdct.c"
    "./fixpnt/real/polyphase.c"
    "./fixpnt/real/scalfact.c"
    "./fixpnt/real/stproc.c"
    "./fixpnt/real/subband.c"
    "./fixpnt/real/trigtabs.c"
)

register_component()