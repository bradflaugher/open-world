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

; ---- land metatile queue: entry = addr lo, addr hi, mt, t0..t3, pad (32 x 8 bytes) ----
; All four tiles of a metatile slot share one 256-byte page (TL low byte <= 222), so only E moves.
_bq_drain::
    ld a,(_bq_budget)
    ld b,a
.bq_loop:
    ld a,(_bq_tail)
    ld c,a
    ld a,(_bq_head)
    cp a,c
    ret z
    ld l,c                  ; hl = bq + tail * 8
    ld h,#0
    add hl,hl
    add hl,hl
    add hl,hl
    ld de,#_bq
    add hl,de
    ld e,(hl)
    inc hl
    ld d,(hl)
    inc hl
    ld a,(hl+)              ; mt
    add a,a
    add a,a
    ld c,a                  ; mt * 4 (attributes)
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
    ld a,(hl)
    ld (de),a
    ld a,(_is_cgb)
    or a,a
    jr z,.bq_next
    ld a,#1
    ldh (rVBK),a
    ld hl,#_mt_a
    ld a,c
    add a,l
    ld l,a
    adc a,h
    sub a,l
    ld h,a
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
.bq_next:
    ld a,(_bq_tail)
    inc a
    and a,#31
    ld (_bq_tail),a
    dec b
    jr nz,.bq_loop
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
