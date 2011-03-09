/*
 * jcapimin.c
 *
 * Copyright (C) 1994-1998, Thomas G. Lane.
 * This file is part of the Independent OE_JPEG Group's software.
 * For conditions of distribution and use, see the accompanying README file.
 *
 * This file contains application interface code for the compression half
 * of the OE_JPEG library.  These are the "minimum" API routines that may be
 * needed in either the normal full-compression case or the transcoding-only
 * case.
 *
 * Most of the routines intended to be called directly by an application
 * are in this file or in jcapistd.c.  But also see jcparam.c for
 * parameter-setup helper routines, jcomapi.c for routines shared by
 * compression and decompression, and jctrans.c for the transcoding case.
 */

#define OE_JPEG_INTERNALS
#include "OpenExif_jinclude.h"
#include "OpenExifJpegLib.h"


/*
 * Initialization of a OE_JPEG compression object.
 * The error manager must already be set up (in case memory manager fails).
 */

GLOBAL(void)
openexif_jpeg_CreateCompress (oe_j_compress_ptr cinfo, int version, size_t structsize)
{
  int i;

  /* Guard against version mismatches between library and caller. */
  cinfo->mem = NULL;		/* so openexif_jpeg_destroy knows mem mgr not called */
  if (version != OPENEXIF_JPEG_LIB_VERSION)
    ERREXIT2(cinfo, OE_JERR_BAD_LIB_VERSION, OPENEXIF_JPEG_LIB_VERSION, version);
  if (structsize != SIZEOF(struct openexif_jpeg_compress_struct))
    ERREXIT2(cinfo, OE_JERR_BAD_STRUCT_SIZE, 
	     (int) SIZEOF(struct openexif_jpeg_compress_struct), (int) structsize);

  /* For debugging purposes, we zero the whole master structure.
   * But the application has already set the err pointer, and may have set
   * client_data, so we have to save and restore those fields.
   * Note: if application hasn't set client_data, tools like Purify may
   * complain here.
   */
  {
    struct openexif_jpeg_error_mgr * err = cinfo->err;
    void * client_data = cinfo->client_data; /* ignore Purify complaint here */
    MEMZERO(cinfo, SIZEOF(struct openexif_jpeg_compress_struct));
    cinfo->err = err;
    cinfo->client_data = client_data;
  }
  cinfo->is_decompressor = FALSE;

  /* Initialize a memory manager instance for this object */
  openexif_jinit_memory_mgr((oe_j_common_ptr) cinfo);

  /* Zero out pointers to permanent structures. */
  cinfo->progress = NULL;
  cinfo->dest = NULL;

  cinfo->comp_info = NULL;

  for (i = 0; i < NUM_QUANT_TBLS; i++)
    cinfo->quant_tbl_ptrs[i] = NULL;

  for (i = 0; i < NUM_HUFF_TBLS; i++) {
    cinfo->dc_huff_tbl_ptrs[i] = NULL;
    cinfo->ac_huff_tbl_ptrs[i] = NULL;
  }

  cinfo->script_space = NULL;

  cinfo->input_gamma = 1.0;	/* in case application forgets */

  /* OK, I'm ready */
  cinfo->global_state = CSTATE_START;
}


/*
 * Destruction of a OE_JPEG compression object
 */

GLOBAL(void)
openexif_jpeg_destroy_compress (oe_j_compress_ptr cinfo)
{
  openexif_jpeg_destroy((oe_j_common_ptr) cinfo); /* use common routine */
}


/*
 * Abort processing of a OE_JPEG compression operation,
 * but don't destroy the object itself.
 */

GLOBAL(void)
openexif_jpeg_abort_compress (oe_j_compress_ptr cinfo)
{
  openexif_jpeg_abort((oe_j_common_ptr) cinfo); /* use common routine */
}


/*
 * Forcibly suppress or un-suppress all quantization and Huffman tables.
 * Marks all currently defined tables as already written (if suppress)
 * or not written (if !suppress).  This will control whether they get emitted
 * by a subsequent openexif_jpeg_start_compress call.
 *
 * This routine is exported for use by applications that want to produce
 * abbreviated OE_JPEG datastreams.  It logically belongs in jcparam.c, but
 * since it is called by openexif_jpeg_start_compress, we put it here --- otherwise
 * jcparam.o would be linked whether the application used it or not.
 */

GLOBAL(void)
openexif_jpeg_suppress_tables (oe_j_compress_ptr cinfo, boolean suppress)
{
  int i;
  OE_JQUANT_TBL * qtbl;
  OE_JHUFF_TBL * htbl;

  for (i = 0; i < NUM_QUANT_TBLS; i++) {
    if ((qtbl = cinfo->quant_tbl_ptrs[i]) != NULL)
      qtbl->sent_table = suppress;
  }

  for (i = 0; i < NUM_HUFF_TBLS; i++) {
    if ((htbl = cinfo->dc_huff_tbl_ptrs[i]) != NULL)
      htbl->sent_table = suppress;
    if ((htbl = cinfo->ac_huff_tbl_ptrs[i]) != NULL)
      htbl->sent_table = suppress;
  }
}


/*
 * Finish OE_JPEG compression.
 *
 * If a multipass operating mode was selected, this may do a great deal of
 * work including most of the actual output.
 */

GLOBAL(void)
openexif_jpeg_finish_compress (oe_j_compress_ptr cinfo)
{
  OE_JDIMENSION iMCU_row;

  if (cinfo->global_state == CSTATE_SCANNING ||
      cinfo->global_state == CSTATE_RAW_OK) {
    /* Terminate first pass */
    if (cinfo->next_scanline < cinfo->image_height)
      ERREXIT(cinfo, OE_JERR_TOO_LITTLE_DATA);
    (*cinfo->master->finish_pass) (cinfo);
  } else if (cinfo->global_state != CSTATE_WRCOEFS)
    ERREXIT1(cinfo, OE_JERR_BAD_STATE, cinfo->global_state);
  /* Perform any remaining passes */
  while (! cinfo->master->is_last_pass) {
    (*cinfo->master->prepare_for_pass) (cinfo);
    for (iMCU_row = 0; iMCU_row < cinfo->total_iMCU_rows; iMCU_row++) {
      if (cinfo->progress != NULL) {
	cinfo->progress->pass_counter = (long) iMCU_row;
	cinfo->progress->pass_limit = (long) cinfo->total_iMCU_rows;
	(*cinfo->progress->progress_monitor) ((oe_j_common_ptr) cinfo);
      }
      /* We bypass the main controller and invoke coef controller directly;
       * all work is being done from the coefficient buffer.
       */
      if (! (*cinfo->coef->compress_data) (cinfo, (OE_JSAMPIMAGE) NULL))
	ERREXIT(cinfo, OE_JERR_CANT_SUSPEND);
    }
    (*cinfo->master->finish_pass) (cinfo);
  }
  /* Write EOI, do final cleanup */
  (*cinfo->marker->write_file_trailer) (cinfo);
  (*cinfo->dest->term_destination) (cinfo);
  /* We can use openexif_jpeg_abort to release memory and reset global_state */
  openexif_jpeg_abort((oe_j_common_ptr) cinfo);
}


/*
 * Write a special marker.
 * This is only recommended for writing COM or APPn markers.
 * Must be called after openexif_jpeg_start_compress() and before
 * first call to openexif_jpeg_write_scanlines() or openexif_jpeg_write_raw_data().
 */

GLOBAL(void)
openexif_jpeg_write_marker (oe_j_compress_ptr cinfo, int marker,
		   const OE_JOCTET *dataptr, unsigned int datalen)
{
  OE_JMETHOD(void, write_marker_byte, (oe_j_compress_ptr info, int val));

  if (cinfo->next_scanline != 0 ||
      (cinfo->global_state != CSTATE_SCANNING &&
       cinfo->global_state != CSTATE_RAW_OK &&
       cinfo->global_state != CSTATE_WRCOEFS))
    ERREXIT1(cinfo, OE_JERR_BAD_STATE, cinfo->global_state);

  (*cinfo->marker->write_marker_header) (cinfo, marker, datalen);
  write_marker_byte = cinfo->marker->write_marker_byte;	/* copy for speed */
  while (datalen--) {
    (*write_marker_byte) (cinfo, *dataptr);
    dataptr++;
  }
}

/* Same, but piecemeal. */

GLOBAL(void)
openexif_jpeg_write_m_header (oe_j_compress_ptr cinfo, int marker, unsigned int datalen)
{
  if (cinfo->next_scanline != 0 ||
      (cinfo->global_state != CSTATE_SCANNING &&
       cinfo->global_state != CSTATE_RAW_OK &&
       cinfo->global_state != CSTATE_WRCOEFS))
    ERREXIT1(cinfo, OE_JERR_BAD_STATE, cinfo->global_state);

  (*cinfo->marker->write_marker_header) (cinfo, marker, datalen);
}

GLOBAL(void)
openexif_jpeg_write_m_byte (oe_j_compress_ptr cinfo, int val)
{
  (*cinfo->marker->write_marker_byte) (cinfo, val);
}


/*
 * Alternate compression function: just write an abbreviated table file.
 * Before calling this, all parameters and a data destination must be set up.
 *
 * To produce a pair of files containing abbreviated tables and abbreviated
 * image data, one would proceed as follows:
 *
 *		initialize OE_JPEG object
 *		set OE_JPEG parameters
 *		set destination to table file
 *		openexif_jpeg_write_tables(cinfo);
 *		set destination to image file
 *		openexif_jpeg_start_compress(cinfo, FALSE);
 *		write data...
 *		openexif_jpeg_finish_compress(cinfo);
 *
 * openexif_jpeg_write_tables has the side effect of marking all tables written
 * (same as openexif_jpeg_suppress_tables(..., TRUE)).  Thus a subsequent start_compress
 * will not re-emit the tables unless it is passed write_all_tables=TRUE.
 */

GLOBAL(void)
openexif_jpeg_write_tables (oe_j_compress_ptr cinfo)
{
  if (cinfo->global_state != CSTATE_START)
    ERREXIT1(cinfo, OE_JERR_BAD_STATE, cinfo->global_state);

  /* (Re)initialize error mgr and destination modules */
  (*cinfo->err->reset_error_mgr) ((oe_j_common_ptr) cinfo);
  (*cinfo->dest->init_destination) (cinfo);
  /* Initialize the marker writer ... bit of a crock to do it here. */
  jinit_marker_writer(cinfo);
  /* Write them tables! */
  (*cinfo->marker->write_tables_only) (cinfo);
  /* And clean up. */
  (*cinfo->dest->term_destination) (cinfo);
  /*
   * In library releases up through v6a, we called openexif_jpeg_abort() here to free
   * any working memory allocated by the destination manager and marker
   * writer.  Some applications had a problem with that: they allocated space
   * of their own from the library memory manager, and didn't want it to go
   * away during write_tables.  So now we do nothing.  This will cause a
   * memory leak if an app calls write_tables repeatedly without doing a full
   * compression cycle or otherwise resetting the OE_JPEG object.  However, that
   * seems less bad than unexpectedly freeing memory in the normal case.
   * An app that prefers the old behavior can call openexif_jpeg_abort for itself after
   * each call to openexif_jpeg_write_tables().
   */
}
