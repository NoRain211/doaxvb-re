# Writes PL_MPEG_HEADER, a copy of pl_mpeg.h from PL_MPEG_SOURCE that decodes
# CRI Sofdec movies: MPEG-1 video with 9 to 11 bit intra DC precision, signalled
# in TMPGEnc user data, and quant matrices that change in repeated sequence
# headers. Generic fixes, suitable for upstream.
file(READ "${PL_MPEG_SOURCE}" _pl_mpeg_text)
function(pl_mpeg_replace old new)
    string(FIND "${_pl_mpeg_text}" "${old}" at)
    if(at EQUAL -1)
        message(FATAL_ERROR "pl_mpeg.h changed; update pl_mpeg_sofdec.cmake")
    endif()
    string(REPLACE "${old}" "${new}" _pl_mpeg_text "${_pl_mpeg_text}")
    set(_pl_mpeg_text "${_pl_mpeg_text}" PARENT_SCOPE)
endfunction()
pl_mpeg_replace([=[	{       0,    8}, {      -1,    0},  //   8: 1111 11x
};]=] [=[	{       0,    8}, {  9 << 1,    0},  //   8: 1111 11x
	{       0,    9}, { 10 << 1,    0},  //   9: 1111 111x
	{       0,   10}, {       0,   11},  //  10: 1111 1111x
};]=])
pl_mpeg_replace([=[	{       0,    8}, {      -1,    0},  //   8: 1111 111x
};]=] [=[	{       0,    8}, {  9 << 1,    0},  //   8: 1111 111x
	{       0,    9}, { 10 << 1,    0},  //   9: 1111 1111x
	{       0,   10}, {       0,   11},  //  10: 1111 1111 1x
};]=])
pl_mpeg_replace([=[struct plm_video_t {
	double framerate;]=] [=[struct plm_video_t {
	double framerate;
	int intra_dc_precision;]=])
pl_mpeg_replace([=[self->dc_predictor[0] = 128;]=] [=[self->dc_predictor[0] = 128 << self->intra_dc_precision;]=])
pl_mpeg_replace([=[self->dc_predictor[1] = 128;]=] [=[self->dc_predictor[1] = 128 << self->intra_dc_precision;]=])
pl_mpeg_replace([=[self->dc_predictor[2] = 128;]=] [=[self->dc_predictor[2] = 128 << self->intra_dc_precision;]=])
pl_mpeg_replace([=[self->block_data[0] <<= (3 + 5);]=] [=[self->block_data[0] <<= (3 - self->intra_dc_precision + 5);]=])
pl_mpeg_replace([=[void plm_video_decode_picture(plm_video_t *self);]=] [=[void plm_video_decode_picture(plm_video_t *self);
void plm_video_decode_user_data(plm_video_t *self);]=])
pl_mpeg_replace([=[	// Find first slice start code; skip extension and user data
	do {
		self->start_code = plm_buffer_next_start_code(self->buffer);
	} while (]=] [=[	// Find first slice start code; skip extension and user data
	do {
		self->start_code = plm_buffer_next_start_code(self->buffer);
		if (self->start_code == PLM_START_USER_DATA) {
			plm_video_decode_user_data(self);
		}
	} while (]=])
pl_mpeg_replace([=[void plm_video_init_frame(plm_video_t *self, plm_frame_t *frame, uint8_t *base) {]=] [=[// TMPGEnc-style MPEG-1 streams, such as CRI Sofdec movies, carry 9 to 11 bit
// intra DC precision in picture user data: "IDCPREC", a NUL, an 8-digit length
// and an 8-digit value from 0 to 3, as in MPEG-2's intra_dc_precision.
void plm_video_decode_user_data(plm_video_t *self) {
	static const char tag[8] = "IDCPREC";
	char window[24] = {0};
	for (int i = 0; i < 256 && plm_buffer_has(self->buffer, 24); i++) {
		int next = plm_buffer_read(self->buffer, 24);
		self->buffer->bit_index -= 24;
		if (next == 0x000001) {
			return;
		}
		memmove(window, window + 1, sizeof(window) - 1);
		window[sizeof(window) - 1] = (char)plm_buffer_read(self->buffer, 8);
		if (memcmp(window, tag, sizeof(tag)) != 0) {
			continue;
		}
		int value = 0;
		for (int d = 16; d < 24; d++) {
			if (window[d] < '0' || window[d] > '9') {
				return;
			}
			value = value * 10 + (window[d] - '0');
		}
		if (value <= 3) {
			self->intra_dc_precision = value;
		}
		return;
	}
}

void plm_video_init_frame(plm_video_t *self, plm_frame_t *frame, uint8_t *base) {]=])
pl_mpeg_replace([=[			self->start_code = plm_buffer_find_start_code(self->buffer, PLM_START_PICTURE);
]=] [=[			// Read sequence headers and user data on the way, including one
			// already found after the last slice.
			while (TRUE) {
				if (self->start_code == PLM_START_SEQUENCE) {
					plm_video_decode_sequence_header(self);
				}
				else if (self->start_code == PLM_START_USER_DATA) {
					plm_video_decode_user_data(self);
				}
				self->start_code = plm_buffer_next_start_code(self->buffer);
				if (self->start_code == PLM_START_PICTURE || self->start_code == -1) {
					break;
				}
			}
]=])
pl_mpeg_replace([=[	self->mb_width = (self->width + 15) >> 4;
	self->mb_height]=] [=[	// Repeated sequence headers can change the quant matrices; keep the frames.
	if (self->has_sequence_header) {
		self->width = self->frame_current.width;
		self->height = self->frame_current.height;
		self->start_code = -1;
		return TRUE;
	}

	self->mb_width = (self->width + 15) >> 4;
	self->mb_height]=])
pl_mpeg_replace([=[	self->has_sequence_header = TRUE;
	return TRUE;]=] [=[	self->has_sequence_header = TRUE;
	self->start_code = -1; // consumed
	return TRUE;]=])
# pl_mpeg.h has no @ or generator expressions, so CONFIGURE copies it verbatim
# and rewrites the header only when it changes.
file(CONFIGURE OUTPUT "${PL_MPEG_HEADER}" CONTENT "${_pl_mpeg_text}" @ONLY NEWLINE_STYLE UNIX)
