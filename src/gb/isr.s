; isr.s - OPEN WORLD hot interrupt code (bank 0, hand-written SM83).
;
; _stat_isr : raw STAT vector (no GBDK dispatcher). Fires at LYC = 23 and switches the screen
;             from the horizon band (map 0x9C00) to the land (map 0x9800) during the HBlank of
;             line 23, so line 24 is the first land line with no glitch.
; _bq_drain : VBlank: write queued land metatiles (4 tiles from the entry, + 4 CGB attributes
;             of its metatile) to map 0x9800.
; _vq_drain : VBlank: write queued single tiles (+ attributes) anywhere in the BG maps.

    .module isr

rLCDC = 0x40
rSTAT = 0x41
rSCY  = 0x42
rSCX  = 0x43
rBGP  = 0x47
rVBK  = 0x4F

    .globl _land_scx, _land_scy, _land_bgp
    .globl _bq, _bq_head, _bq_tail, _bq_budget
    .globl _vq, _vq_head, _vq_tail
    .globl _mt_t, _mt_a, _is_cgb

    .area _HOME

_stat_isr::
    push af
1$:
    ldh a,(rSTAT)           ; wait for HBlank of line 23
    and a,#3
    jr nz,1$
    ldh a,(rLCDC)
    and a,#0xF7             ; BG map 0x9800
    ldh (rLCDC),a
    ld a,(_land_scx)
    ldh (rSCX),a
    ld a,(_land_scy)
    ldh (rSCY),a
    ld a,(_land_bgp)
    ldh (rBGP),a
    pop af
    reti

; ---- VBlank VRAM work ----
; Every routine here runs from the VBlank handler with interrupts off and must finish inside
; the VBlank: on hardware a VRAM (or CGB palette) write during mode 3 is simply dropped, which
; leaves a stale tile on screen for good. The handler does not start at line 144 sharp (the
; OAM DMA and the scroll latch come first, and a critical section can hold the interrupt off),
; so a fixed number of entries per frame is not safe: each queue entry is started only while
; LY is 144..152. An entry takes well under a line at either CPU speed, so the last write
; lands before line 153 ends. (LY reads 0 for most of line 153: that stops the loop too.)
rLY   = 0x44
VB_LAST = 9                 ; entries start while LY - 144 < VB_LAST

; ---- land metatile queue: entry = addr lo, addr hi, mt, t0..t3, pad (32 x 8 bytes) ----
; All four tiles of a metatile slot share one 256-byte page (TL low byte <= 222), so only E moves.
_bq_drain::
    ld a,(_bq_budget)
    ld b,a
    ld a,(_bq_tail)
    ld c,a
    ld l,a                  ; hl = bq + tail * 8
    ld h,#0
    add hl,hl
    add hl,hl
    add hl,hl
    ld de,#_bq
    add hl,de
.bq_loop:
    ld a,(_bq_head)
    cp a,c
    jr z,.bq_done
    ldh a,(rLY)
    sub a,#144
    cp a,#VB_LAST
    jr nc,.bq_done
    ld a,(hl+)
    ld e,a
    ld a,(hl+)
    ld d,a
    inc hl                  ; mt (attributes: below)
    ld a,(hl+)              ; the four tiles, from the entry
    ld (de),a
    inc e
    ld a,(hl+)
    ld (de),a
    ld a,e
    add a,#31
    ld e,a
    ld a,(hl+)
    ld (de),a
    inc e
    ld a,(hl+)
    ld (de),a
    ld a,(_is_cgb)
    or a,a
    jr z,.bq_next
    push hl
    dec hl
    dec hl
    dec hl
    dec hl
    dec hl
    ld a,(hl)               ; mt
    add a,a
    add a,a                 ; mt * 4 (MT_COUNT <= 64)
    ld hl,#_mt_a
    add a,l
    ld l,a
    adc a,h
    sub a,l
    ld h,a
    ld a,#1
    ldh (rVBK),a
    ld a,e
    sub a,#33
    ld e,a
    ld a,(hl+)
    ld (de),a
    inc e
    ld a,(hl+)
    ld (de),a
    ld a,e
    add a,#31
    ld e,a
    ld a,(hl+)
    ld (de),a
    inc e
    ld a,(hl)
    ld (de),a
    xor a,a
    ldh (rVBK),a
    pop hl
.bq_next:
    inc hl                  ; pad
    ld a,c
    inc a
    and a,#31
    ld c,a
    jr nz,.bq_nowrap
    ld hl,#_bq
.bq_nowrap:
    dec b
    jr nz,.bq_loop
.bq_done:
    ld a,c
    ld (_bq_tail),a
    ret

; ---- single tile queue: entry = addr lo, addr hi, tile, attr (32 entries), up to 8 per call ----
_vq_drain::
    ld b,#8
.vq_loop:
    ld a,(_vq_tail)
    ld c,a
    ld a,(_vq_head)
    cp a,c
    ret z
    ldh a,(rLY)
    sub a,#144
    cp a,#VB_LAST
    ret nc
    ld a,c
    add a,a
    add a,a
    ld hl,#_vq
    add a,l
    ld l,a
    adc a,h
    sub a,l
    ld h,a
    ld e,(hl)
    inc hl
    ld d,(hl)
    inc hl
    ld a,(hl+)
    ld (de),a
    ld a,(_is_cgb)
    or a,a
    jr z,.vq_next
    ld a,#1
    ldh (rVBK),a
    ld a,(hl)
    ld (de),a
    xor a,a
    ldh (rVBK),a
.vq_next:
    ld a,(_vq_tail)
    inc a
    and a,#31
    ld (_vq_tail),a
    dec b
    jr nz,.vq_loop
    ret

; ---- CGB: all 8 palettes (64 bytes) from vb_src to the palette port vb_reg (0x68 BG,
; 0x6A OBJ). About 1.4 lines at double speed; the caller checks there is room. ----
    .globl _vb_src, _vb_dst, _vb_reg
_pal_put::
    ld a,(_vb_src)
    ld l,a
    ld a,(_vb_src+1)
    ld h,a
    ld a,(_vb_reg)
    ld c,a
    ld a,#0x80              ; index 0, auto-increment
    ldh (c),a
    inc c
    ld b,#16
1$:
    ld a,(hl+)
    ldh (c),a
    ld a,(hl+)
    ldh (c),a
    ld a,(hl+)
    ldh (c),a
    ld a,(hl+)
    ldh (c),a
    dec b
    jr nz,1$
    ret

; ---- one animated tile: 16 bytes from vb_src to vb_dst (under a line on a DMG) ----
_tile_put::
    ld a,(_vb_src)
    ld e,a
    ld a,(_vb_src+1)
    ld d,a
    ld a,(_vb_dst)
    ld l,a
    ld a,(_vb_dst+1)
    ld h,a
    ld b,#4
1$:
    ld a,(de)
    ld (hl+),a
    inc de
    ld a,(de)
    ld (hl+),a
    inc de
    ld a,(de)
    ld (hl+),a
    inc de
    ld a,(de)
    ld (hl+),a
    inc de
    dec b
    jr nz,1$
    ret

; ---- main-loop VRAM writes with the LCD on ----
; The usual "wait until STAT says mode 0/1, then write" is only safe if nothing can run between
; the check and the write: an interrupt taken there (the VBlank handler runs the sound engine
; and can return well after the VBlank) puts the write in mode 3, where hardware drops it.
; So: wait with interrupts on (so the wait never delays one), then check again with them off
; and write at once. Two bytes per check: both land within 10 M-cycles of the STAT read, and
; after the last cycle of an HBlank VRAM stays open for the 20 cycles of mode 2. Interrupts
; are off for about 12 cycles at a time.
; mc_n pairs (1..255) from mc_src to mc_dst.
    .globl _mc_src, _mc_dst, _mc_n
_vram_pairs::
    ld a,(_mc_src)
    ld e,a
    ld a,(_mc_src+1)
    ld d,a
    ld a,(_mc_dst)
    ld l,a
    ld a,(_mc_dst+1)
    ld h,a
1$:
    ld a,(de)
    ld b,a
    inc de
    ld a,(de)
    ld c,a
    inc de
2$:
    ldh a,(rSTAT)
    and a,#2
    jr nz,2$
    di
    ldh a,(rSTAT)
    and a,#2
    jr nz,3$
    ld (hl),b
    inc hl
    ld (hl),c
    ei
    inc hl
    ld a,(_mc_n)
    dec a
    ld (_mc_n),a
    jr nz,1$
    ret
3$:
    ei
    jr 2$

; one byte: mc_n to mc_dst
_vram_byte::
    ld a,(_mc_dst)
    ld l,a
    ld a,(_mc_dst+1)
    ld h,a
    ld a,(_mc_n)
    ld b,a
1$:
    ldh a,(rSTAT)
    and a,#2
    jr nz,1$
    di
    ldh a,(rSTAT)
    and a,#2
    jr nz,2$
    ld (hl),b
    ei
    ret
2$:
    ei
    jr 1$

; ---- save checksum: a += byte, b += a over cks_len bytes at cks_ptr (13 cycles/byte) ----
    .globl _cks_ptr, _cks_len, _cks_a, _cks_b
_cks_run::
    ld a,(_cks_ptr)
    ld l,a
    ld a,(_cks_ptr+1)
    ld h,a
    ld a,(_cks_len)
    ld c,a
    ld a,(_cks_len+1)
    ld b,a
    ld a,(_cks_a)
    ld d,a
    ld a,(_cks_b)
    ld e,a
    ld a,b
    or a,c
    jr z,.cks_done
.cks_loop:
    ld a,(hl+)
    add a,d
    ld d,a
    add a,e
    ld e,a
    dec bc
    ld a,b
    or a,c
    jr nz,.cks_loop
.cks_done:
    ld a,d
    ld (_cks_a),a
    ld a,e
    ld (_cks_b),a
    ret

; ---- stack paint: fill free WRAM between the end of the data (s__HEAP) and SP - 32 with
; 0xA5 at boot, so the tests can find the stack's high-water mark ----
    .globl s__HEAP
    .globl _dbg_ram_end
_stack_paint::
    ld hl,#s__HEAP
    ld a,l
    ld (_dbg_ram_end),a
    ld a,h
    ld (_dbg_ram_end+1),a
    ld hl,#-32
    add hl,sp
    ld b,h
    ld c,l                  ; bc = the end of the paint (SP - 32)
    ld hl,#s__HEAP
1$:
    ld a,#0xA5
    ld (hl+),a
    ld a,l
    cp a,c
    jr nz,1$
    ld a,h
    cp a,b
    jr nz,1$
    ret
