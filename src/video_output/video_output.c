/******************************************************************************
 * video_output.c : video output thread
 * (c)2000 VideoLAN
 ******************************************************************************
 * This module describes the programming interface for video output threads.
 * It includes functions allowing to open a new thread, send pictures to a
 * thread, and destroy a previously oppenned video output thread.
 ******************************************************************************/

/******************************************************************************
 * Preamble
 ******************************************************************************/
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "common.h"
#include "config.h"
#include "mtime.h"
#include "vlc_thread.h"
#include "video.h"
#include "video_output.h"
#include "video_text.h"
#include "video_sys.h"
#include "video_yuv.h"
#include "intf_msg.h"
#include "main.h"

/******************************************************************************
 * Local prototypes
 ******************************************************************************/
static int      InitThread              ( vout_thread_t *p_vout );
static void     RunThread               ( vout_thread_t *p_vout );
static void     ErrorThread             ( vout_thread_t *p_vout );
static void     EndThread               ( vout_thread_t *p_vout );
static void     DestroyThread           ( vout_thread_t *p_vout, int i_status );
static void     Print                   ( vout_thread_t *p_vout, int i_x, int i_y, int i_halign, int i_valign, unsigned char *psz_text );

static void     SetBufferArea           ( vout_thread_t *p_vout, int i_x, int i_y, int i_w, int i_h );
static void     SetBufferPicture        ( vout_thread_t *p_vout, picture_t *p_pic );
static void     RenderPicture           ( vout_thread_t *p_vout, picture_t *p_pic );
static void     RenderPictureInfo       ( vout_thread_t *p_vout, picture_t *p_pic );
static void     RenderSubPictureUnit    ( vout_thread_t *p_vout, spu_t *p_spu );
static void     RenderInterface         ( vout_thread_t *p_vout );
static void     RenderIdle              ( vout_thread_t *p_vout );
static void     RenderInfo              ( vout_thread_t *p_vout );
static int      Manage                  ( vout_thread_t *p_vout );

/******************************************************************************
 * vout_CreateThread: creates a new video output thread
 ******************************************************************************
 * This function creates a new video output thread, and returns a pointer
 * to its description. On error, it returns NULL.
 * If pi_status is NULL, then the function will block until the thread is ready.
 * If not, it will be updated using one of the THREAD_* constants.
 ******************************************************************************/
vout_thread_t * vout_CreateThread               ( char *psz_display, int i_root_window, 
                                                  int i_width, int i_height, int *pi_status )
{
    vout_thread_t * p_vout;                              /* thread descriptor */
    int             i_status;                                /* thread status */
    int             i_index;                /* index for array initialization */    

    /* Allocate descriptor */
    intf_DbgMsg("\n");    
    p_vout = (vout_thread_t *) malloc( sizeof(vout_thread_t) );
    if( p_vout == NULL )
    {
        intf_ErrMsg("error: %s\n", strerror(ENOMEM));        
        return( NULL );
    }

    /* Initialize thread properties - thread id and locks will be initialized 
     * later */
    p_vout->b_die               = 0;
    p_vout->b_error             = 0;    
    p_vout->b_active            = 0;
    p_vout->pi_status           = (pi_status != NULL) ? pi_status : &i_status;
    *p_vout->pi_status          = THREAD_CREATE;    

    /* Initialize some fields used by the system-dependant method - these fields will
     * probably be modified by the method, and are only preferences */
    p_vout->i_width             = i_width;
    p_vout->i_height            = i_height;
    p_vout->i_bytes_per_line    = i_width * 2;    
    p_vout->i_screen_depth      = 15;
    p_vout->i_bytes_per_pixel   = 2;
    p_vout->f_gamma             = VOUT_GAMMA;    

    p_vout->b_grayscale         = main_GetIntVariable( VOUT_GRAYSCALE_VAR, 
                                                       VOUT_GRAYSCALE_DEFAULT );
    p_vout->b_info              = 0;    
    p_vout->b_interface         = 0;
    p_vout->b_scale             = 0;
    
    intf_DbgMsg("wished configuration: %dx%d,%d (%d bytes/pixel, %d bytes/line)\n",
                p_vout->i_width, p_vout->i_height, p_vout->i_screen_depth,
                p_vout->i_bytes_per_pixel, p_vout->i_bytes_per_line );

#ifdef STATS
    /* Initialize statistics fields */
    p_vout->render_time         = 0;    
    p_vout->c_fps_samples       = 0;    
#endif      

    /* Initialize running properties */
    p_vout->i_changes           = 0;
    p_vout->last_picture_date   = 0;
    p_vout->last_display_date   = 0;

    /* Initialize buffer index */
    p_vout->i_buffer_index      = 0;

    /* Initialize pictures and spus - translation tables and functions
     * will be initialized later in InitThread */    
    for( i_index = 0; i_index < VOUT_MAX_PICTURES; i_index++)
    {
        p_vout->p_picture[i_index].i_type   = EMPTY_PICTURE;
        p_vout->p_picture[i_index].i_status = FREE_PICTURE;
        p_vout->p_spu[i_index].i_type  = EMPTY_SPU;
        p_vout->p_spu[i_index].i_status= FREE_SPU;
    }
   
    /* Create and initialize system-dependant method - this function issues its
     * own error messages */
    if( vout_SysCreate( p_vout, psz_display, i_root_window ) )
    {
      free( p_vout );
      return( NULL );
    }
    intf_DbgMsg("actual configuration: %dx%d,%d (%d bytes/pixel, %d bytes/line)\n",
                p_vout->i_width, p_vout->i_height, p_vout->i_screen_depth,
                p_vout->i_bytes_per_pixel, p_vout->i_bytes_per_line );

    /* Load fonts - fonts must be initialized after the systme method since
     * they may be dependant of screen depth and other thread properties */
    p_vout->p_default_font      = vout_LoadFont( VOUT_DEFAULT_FONT );    
    if( p_vout->p_default_font == NULL )
    {
        vout_SysDestroy( p_vout );        
        free( p_vout );        
        return( NULL );        
    }    
    p_vout->p_large_font        = vout_LoadFont( VOUT_LARGE_FONT );        
    if( p_vout->p_large_font == NULL )
    {
        vout_UnloadFont( p_vout->p_default_font );        
        vout_SysDestroy( p_vout );        
        free( p_vout );        
        return( NULL );        
    }     

    /* Create thread and set locks */
    vlc_mutex_init( &p_vout->picture_lock );
    vlc_mutex_init( &p_vout->spu_lock );    
    vlc_mutex_init( &p_vout->change_lock );    
    vlc_mutex_lock( &p_vout->change_lock );    
    if( vlc_thread_create( &p_vout->thread_id, "video output", (void *) RunThread, (void *) p_vout) )
    {
        intf_ErrMsg("error: %s\n", strerror(ENOMEM));
        vout_UnloadFont( p_vout->p_default_font );
        vout_UnloadFont( p_vout->p_large_font );        
	vout_SysDestroy( p_vout );
        free( p_vout );
        return( NULL );
    }   

    intf_Msg("Video display initialized (%dx%d, %d bpp)\n", 
             p_vout->i_width, p_vout->i_height, p_vout->i_screen_depth );    

    /* If status is NULL, wait until the thread is created */
    if( pi_status == NULL )
    {
        do
        {            
            msleep( THREAD_SLEEP );
        }while( (i_status != THREAD_READY) && (i_status != THREAD_ERROR) 
                && (i_status != THREAD_FATAL) );
        if( i_status != THREAD_READY )
        {
            return( NULL );            
        }        
    }
    return( p_vout );
}

/******************************************************************************
 * vout_DestroyThread: destroys a previously created thread
 ******************************************************************************
 * Destroy a terminated thread. 
 * The function will request a destruction of the specified thread. If pi_error
 * is NULL, it will return once the thread is destroyed. Else, it will be 
 * update using one of the THREAD_* constants.
 ******************************************************************************/
void vout_DestroyThread( vout_thread_t *p_vout, int *pi_status )
{  
    int     i_status;                                        /* thread status */

    /* Set status */
    intf_DbgMsg("\n");
    p_vout->pi_status = (pi_status != NULL) ? pi_status : &i_status;
    *p_vout->pi_status = THREAD_DESTROY;    
     
    /* Request thread destruction */
    p_vout->b_die = 1;

    /* If status is NULL, wait until thread has been destroyed */
    if( pi_status == NULL )
    {
        do
        {
            msleep( THREAD_SLEEP );
        }while( (i_status != THREAD_OVER) && (i_status != THREAD_ERROR) 
                && (i_status != THREAD_FATAL) );   
    }
}

/******************************************************************************
 * vout_DisplaySubPictureUnit: display a sub picture unit
 ******************************************************************************
 * Remove the reservation flag of an spu, which will cause it to be ready for
 * display. The picture does not need to be locked, since it is ignored by
 * the output thread if is reserved.
 ******************************************************************************/
void  vout_DisplaySubPictureUnit( vout_thread_t *p_vout, spu_t *p_spu )
{
#ifdef DEBUG_VIDEO
    char        psz_begin_date[MSTRTIME_MAX_SIZE];  /* buffer for date string */
    char        psz_end_date[MSTRTIME_MAX_SIZE];    /* buffer for date string */
#endif

#ifdef DEBUG
    /* Check if status is valid */
    if( p_spu->i_status != RESERVED_SPU )
    {
        intf_DbgMsg("error: spu %p has invalid status %d\n", p_spu, p_spu->i_status );       
    }   
#endif

    /* Remove reservation flag */
    p_spu->i_status = READY_SPU;

#ifdef DEBUG_VIDEO
    /* Send subpicture informations */
    intf_DbgMsg("spu %p: type=%d, begin date=%s, end date=%s\n", p_spu, p_spu->i_type, 
                mstrtime( psz_begin_date, p_spu->begin_date ), 
                mstrtime( psz_end_date, p_spu->end_date ) );    
#endif
}

/******************************************************************************
 * vout_CreateSubPictureUnit: allocate an spu in the video output heap.
 ******************************************************************************
 * This function create a reserved spu in the video output heap. 
 * A null pointer is returned if the function fails. This method provides an
 * already allocated zone of memory in the spu data fields. It needs locking
 * since several pictures can be created by several producers threads. 
 ******************************************************************************/
spu_t *vout_CreateSubPictureUnit( vout_thread_t *p_vout, int i_type, 
                                 int i_size )
{
    //??
}

/******************************************************************************
 * vout_DestroySubPictureUnit: remove a permanent or reserved spu from the heap
 ******************************************************************************
 * This function frees a previously reserved spu.
 * It is meant to be used when the construction of a picture aborted.
 * This function does not need locking since reserved spus are ignored by
 * the output thread.
 ******************************************************************************/
void vout_DestroySubPictureUnit( vout_thread_t *p_vout, spu_t *p_spu )
{
#ifdef DEBUG
   /* Check if spu status is valid */
   if( p_spu->i_status != RESERVED_SPU )
   {
       intf_DbgMsg("error: spu %p has invalid status %d\n", p_spu, p_spu->i_status );       
   }   
#endif

    p_spu->i_status = DESTROYED_SPU;

#ifdef DEBUG_VIDEO
    intf_DbgMsg("spu %p\n", p_spu);    
#endif
}

/******************************************************************************
 * vout_DisplayPicture: display a picture
 ******************************************************************************
 * Remove the reservation flag of a picture, which will cause it to be ready for
 * display. The picture won't be displayed until vout_DatePicture has been 
 * called.
 ******************************************************************************/
void  vout_DisplayPicture( vout_thread_t *p_vout, picture_t *p_pic )
{
    vlc_mutex_lock( &p_vout->picture_lock );
    switch( p_pic->i_status )
    {
    case RESERVED_PICTURE:        
        p_pic->i_status = RESERVED_DISP_PICTURE;
        break;        
    case RESERVED_DATED_PICTURE:
        p_pic->i_status = READY_PICTURE;
        break;        
#ifdef DEBUG
    default:        
        intf_DbgMsg("error: picture %p has invalid status %d\n", p_pic, p_pic->i_status );    
        break;        
#endif
    }

#ifdef DEBUG_VIDEO
    intf_DbgMsg("picture %p\n", p_pic );
#endif

    vlc_mutex_unlock( &p_vout->picture_lock );
}

/******************************************************************************
 * vout_DatePicture: date a picture
 ******************************************************************************
 * Remove the reservation flag of a picture, which will cause it to be ready for
 * display. The picture won't be displayed until vout_DisplayPicture has been 
 * called.
 ******************************************************************************/
void  vout_DatePicture( vout_thread_t *p_vout, picture_t *p_pic, mtime_t date )
{
    vlc_mutex_lock( &p_vout->picture_lock );
    p_pic->date = date;    
    switch( p_pic->i_status )
    {
    case RESERVED_PICTURE:        
        p_pic->i_status = RESERVED_DATED_PICTURE;
        break;        
    case RESERVED_DISP_PICTURE:
        p_pic->i_status = READY_PICTURE;
        break;        
#ifdef DEBUG
    default:        
        intf_DbgMsg("error: picture %p has invalid status %d\n", p_pic, p_pic->i_status );    
        break;        
#endif
    }

#ifdef DEBUG_VIDEO
    intf_DbgMsg("picture %p\n", p_pic);
#endif

    vlc_mutex_unlock( &p_vout->picture_lock );
}

/******************************************************************************
 * vout_CreatePicture: allocate a picture in the video output heap.
 ******************************************************************************
 * This function create a reserved image in the video output heap. 
 * A null pointer is returned if the function fails. This method provides an
 * already allocated zone of memory in the picture data fields. It needs locking
 * since several pictures can be created by several producers threads. 
 ******************************************************************************/
picture_t *vout_CreatePicture( vout_thread_t *p_vout, int i_type, 
			       int i_width, int i_height )
{
    int         i_picture;                                   /* picture index */
    int         i_chroma_width = 0;                           /* chroma width */    
    picture_t * p_free_picture = NULL;                  /* first free picture */    
    picture_t * p_destroyed_picture = NULL;        /* first destroyed picture */    

    /* Get lock */
    vlc_mutex_lock( &p_vout->picture_lock );

    /* 
     * Look for an empty place 
     */
    for( i_picture = 0; 
         i_picture < VOUT_MAX_PICTURES; 
         i_picture++ )
    {
	if( p_vout->p_picture[i_picture].i_status == DESTROYED_PICTURE )
	{
	    /* Picture is marked for destruction, but is still allocated - note
             * that if width and type are the same for two pictures, chroma_width 
             * should also be the same */
	    if( (p_vout->p_picture[i_picture].i_type           == i_type)   &&
		(p_vout->p_picture[i_picture].i_height         == i_height) &&
		(p_vout->p_picture[i_picture].i_width          == i_width) )
	    {
		/* Memory size do match : memory will not be reallocated, and function
                 * can end immediately - this is the best possible case, since no
                 * memory allocation needs to be done */
		p_vout->p_picture[i_picture].i_status = RESERVED_PICTURE;
#ifdef DEBUG_VIDEO
                intf_DbgMsg("picture %p (in destroyed picture slot)\n", 
                            &p_vout->p_picture[i_picture] );                
#endif
		vlc_mutex_unlock( &p_vout->picture_lock );
		return( &p_vout->p_picture[i_picture] );
	    }
	    else if( p_destroyed_picture == NULL )
	    {
		/* Memory size do not match, but picture index will be kept in
		 * case no other place are left */
		p_destroyed_picture = &p_vout->p_picture[i_picture];                
	    }	    
	}
        else if( (p_free_picture == NULL) && 
                 (p_vout->p_picture[i_picture].i_status == FREE_PICTURE ))
        {
	    /* Picture is empty and ready for allocation */
            p_free_picture = &p_vout->p_picture[i_picture];            
        }
    }

    /* If no free picture is available, use a destroyed picture */
    if( (p_free_picture == NULL) && (p_destroyed_picture != NULL ) )
    { 
	/* No free picture or matching destroyed picture has been found, but
	 * a destroyed picture is still avalaible */
        free( p_destroyed_picture->p_data );        
        p_free_picture = p_destroyed_picture;        
    }

    /*
     * Prepare picture
     */
    if( p_free_picture != NULL )
    {
        /* Allocate memory */
        switch( i_type )
        {
        case YUV_420_PICTURE:          /* YUV 420: 1,1/4,1/4 samples per pixel */
            i_chroma_width = i_width / 2;            
            p_free_picture->p_data = malloc( i_height * i_chroma_width * 3 * sizeof( yuv_data_t ) );
            p_free_picture->p_y = (yuv_data_t *)p_free_picture->p_data;
            p_free_picture->p_u = (yuv_data_t *)p_free_picture->p_data +i_height*i_chroma_width*4/2;
            p_free_picture->p_v = (yuv_data_t *)p_free_picture->p_data +i_height*i_chroma_width*5/2;
            break;
        case YUV_422_PICTURE:          /* YUV 422: 1,1/2,1/2 samples per pixel */
            i_chroma_width = i_width / 2;            
            p_free_picture->p_data = malloc( i_height * i_chroma_width * 4 * sizeof( yuv_data_t ) );
            p_free_picture->p_y = (yuv_data_t *)p_free_picture->p_data;
            p_free_picture->p_u = (yuv_data_t *)p_free_picture->p_data +i_height*i_chroma_width*2;
            p_free_picture->p_v = (yuv_data_t *)p_free_picture->p_data +i_height*i_chroma_width*3;
            break;
        case YUV_444_PICTURE:              /* YUV 444: 1,1,1 samples per pixel */
            i_chroma_width = i_width;            
            p_free_picture->p_data = malloc( i_height * i_chroma_width * 3 * sizeof( yuv_data_t ) );
            p_free_picture->p_y = (yuv_data_t *)p_free_picture->p_data;
            p_free_picture->p_u = (yuv_data_t *)p_free_picture->p_data +i_height*i_chroma_width;
            p_free_picture->p_v = (yuv_data_t *)p_free_picture->p_data +i_height*i_chroma_width*2;
            break;                
#ifdef DEBUG
        default:
            intf_DbgMsg("error: unknown picture type %d\n", i_type );
            p_free_picture->p_data   =  NULL;            
            break;            
#endif    
        }

        if( p_free_picture->p_data != NULL )
        {        
            /* Copy picture informations, set some default values */
            p_free_picture->i_type                      = i_type;
            p_free_picture->i_status                    = RESERVED_PICTURE;
            p_free_picture->i_matrix_coefficients       = 1; 
            p_free_picture->i_width                     = i_width;
            p_free_picture->i_height                    = i_height;
            p_free_picture->i_chroma_width              = i_chroma_width;            
            p_free_picture->i_display_horizontal_offset = 0;
            p_free_picture->i_display_vertical_offset   = 0;            
            p_free_picture->i_display_width             = i_width;
            p_free_picture->i_display_height            = i_height;
            p_free_picture->i_aspect_ratio              = AR_SQUARE_PICTURE;            
            p_free_picture->i_refcount                  = 0;            
        }
        else
        {
            /* Memory allocation failed : set picture as empty */
            p_free_picture->i_type   =  EMPTY_PICTURE;            
            p_free_picture->i_status =  FREE_PICTURE;            
            p_free_picture =            NULL;            
            intf_ErrMsg("warning: %s\n", strerror( ENOMEM ) );            
        }
        
#ifdef DEBUG_VIDEO
        intf_DbgMsg("picture %p (in free picture slot)\n", p_free_picture );        
#endif
        vlc_mutex_unlock( &p_vout->picture_lock );
        return( p_free_picture );
    }
    
    // No free or destroyed picture could be found
    intf_DbgMsg( "warning: heap is full\n" );
    vlc_mutex_unlock( &p_vout->picture_lock );
    return( NULL );
}

/******************************************************************************
 * vout_DestroyPicture: remove a permanent or reserved picture from the heap
 ******************************************************************************
 * This function frees a previously reserved picture or a permanent
 * picture. It is meant to be used when the construction of a picture aborted.
 * Note that the picture will be destroyed even if it is linked !
 * This function does not need locking since reserved pictures are ignored by
 * the output thread.
 ******************************************************************************/
void vout_DestroyPicture( vout_thread_t *p_vout, picture_t *p_pic )
{
#ifdef DEBUG
   /* Check if picture status is valid */
   if( (p_pic->i_status != RESERVED_PICTURE) && 
       (p_pic->i_status != RESERVED_DATED_PICTURE) &&
       (p_pic->i_status != RESERVED_DISP_PICTURE) )
   {
       intf_DbgMsg("error: picture %p has invalid status %d\n", p_pic, p_pic->i_status );       
   }   
#endif

    p_pic->i_status = DESTROYED_PICTURE;

#ifdef DEBUG_VIDEO
    intf_DbgMsg("picture %p\n", p_pic);    
#endif
}

/******************************************************************************
 * vout_LinkPicture: increment reference counter of a picture
 ******************************************************************************
 * This function increment the reference counter of a picture in the video
 * heap. It needs a lock since several producer threads can access the picture.
 ******************************************************************************/
void vout_LinkPicture( vout_thread_t *p_vout, picture_t *p_pic )
{
    vlc_mutex_lock( &p_vout->picture_lock );
    p_pic->i_refcount++;

#ifdef DEBUG_VIDEO
    intf_DbgMsg("picture %p refcount=%d\n", p_pic, p_pic->i_refcount );    
#endif

    vlc_mutex_unlock( &p_vout->picture_lock );
}

/******************************************************************************
 * vout_UnlinkPicture: decrement reference counter of a picture
 ******************************************************************************
 * This function decrement the reference counter of a picture in the video heap.
 ******************************************************************************/
void vout_UnlinkPicture( vout_thread_t *p_vout, picture_t *p_pic )
{
    vlc_mutex_lock( &p_vout->picture_lock );
    p_pic->i_refcount--;

#ifdef DEBUG_VIDEO
    if( p_pic->i_refcount < 0 )
    {
        intf_DbgMsg("error: refcount < 0\n");
        p_pic->i_refcount = 0;        
    }    
#endif

    if( (p_pic->i_refcount == 0) && (p_pic->i_status == DISPLAYED_PICTURE) )
    {
	p_pic->i_status = DESTROYED_PICTURE;
    }

#ifdef DEBUG_VIDEO
    intf_DbgMsg("picture %p refcount=%d\n", p_pic, p_pic->i_refcount );    
#endif

    vlc_mutex_unlock( &p_vout->picture_lock );
}

/******************************************************************************
 * vout_ClearBuffer: clear a whole buffer
 ******************************************************************************
 * This function is called when a buffer is initialized. It clears the whole
 * buffer.
 ******************************************************************************/
void vout_ClearBuffer( vout_thread_t *p_vout, vout_buffer_t *p_buffer )
{
    /* No picture previously */
    p_buffer->i_pic_x =         0;
    p_buffer->i_pic_y =         0;
    p_buffer->i_pic_width =     0;
    p_buffer->i_pic_height =    0;

    /* The first area covers all the screen */
    p_buffer->i_areas =                 1;
    p_buffer->pi_area_begin[0] =        0;
    p_buffer->pi_area_end[0] =          p_vout->i_height - 1;
}

/* following functions are local */

/******************************************************************************
 * InitThread: initialize video output thread
 ******************************************************************************
 * This function is called from RunThread and performs the second step of the
 * initialization. It returns 0 on success. Note that the thread's flag are not
 * modified inside this function.
 ******************************************************************************/
static int InitThread( vout_thread_t *p_vout )
{
    /* Update status */
    intf_DbgMsg("\n");
    *p_vout->pi_status = THREAD_START;    

    /* Initialize output method - this function issues its own error messages */
    if( vout_SysInit( p_vout ) )
    {
        return( 1 );
    } 

    /* Initialize convertion tables and functions */
    if( vout_InitTables( p_vout ) )
    {
        intf_ErrMsg("error: can't allocate translation tables\n");
        return( 1 );                
    }
    
    /* Mark thread as running and return */
    p_vout->b_active =          1;    
    *p_vout->pi_status =        THREAD_READY;    
    intf_DbgMsg("thread ready\n");    
    return( 0 );    
}

/******************************************************************************
 * RunThread: video output thread
 ******************************************************************************
 * Video output thread. This function does only returns when the thread is
 * terminated. It handles the pictures arriving in the video heap and the
 * display device events.
 ******************************************************************************/
static void RunThread( vout_thread_t *p_vout)
{
    int             i_index;                                 /* index in heap */
    mtime_t         current_date;                             /* current date */
    mtime_t         display_date;                             /* display date */    
    boolean_t       b_display;                                /* display flag */    
    picture_t *     p_pic;                                 /* picture pointer */
    spu_t *         p_spu;                              /* subpicture pointer */    
     
    /* 
     * Initialize thread
     */
    p_vout->b_error = InitThread( p_vout );
    if( p_vout->b_error )
    {
        DestroyThread( p_vout, THREAD_ERROR );
        return;        
    }    
    intf_DbgMsg("\n");

    /*
     * Main loop - it is not executed if an error occured during
     * initialization
     */
    while( (!p_vout->b_die) && (!p_vout->b_error) )
    {
        /* Initialize loop variables */
        p_pic =         NULL;
        p_spu =         NULL;
        display_date =  0;        
        current_date =  mdate();

        /* 
	 * Find the picture to display - this operation does not need lock,
         * since only READY_PICTUREs are handled 
         */
        for( i_index = 0; i_index < VOUT_MAX_PICTURES; i_index++ )
	{
	    if( (p_vout->p_picture[i_index].i_status == READY_PICTURE) &&
		( (p_pic == NULL) || 
		  (p_vout->p_picture[i_index].date < display_date) ) )
	    {
                p_pic = &p_vout->p_picture[i_index];
                display_date = p_pic->date;                
	    }
	}
 
        if( p_pic )
        {
#ifdef STATS
            /* Computes FPS rate */
            p_vout->p_fps_sample[ p_vout->c_fps_samples++ % VOUT_FPS_SAMPLES ] = display_date;
#endif	    
	    if( display_date < current_date )
	    {
		/* Picture is late: it will be destroyed and the thread will sleep and
                 * go to next picture */
                vlc_mutex_lock( &p_vout->picture_lock );
                p_pic->i_status = p_pic->i_refcount ? DISPLAYED_PICTURE : DESTROYED_PICTURE;
		intf_DbgMsg( "warning: late picture %p skipped refcount=%d\n", p_pic, p_pic->i_refcount );
                vlc_mutex_unlock( &p_vout->picture_lock );
                p_pic =         NULL;                
                display_date =  0;                
	    }
	    else if( display_date > current_date + VOUT_DISPLAY_DELAY )
	    {
		/* A picture is ready to be rendered, but its rendering date is
		 * far from the current one so the thread will perform an empty loop
		 * as if no picture were found. The picture state is unchanged */
                p_pic =         NULL;                
                display_date =  0;                
	    }
        }

        /*
         * Find the subpicture to display - this operation does not need lock, since
         * only READY_SPUs are handled. If no picture has been selected,
         * display_date will depend on the spu
         */
        //??

        /*
         * Perform rendering, sleep and display rendered picture
         */
        if( p_pic )                            /* picture and perhaps spu */
        {
            b_display = p_vout->b_active;            

            if( b_display )
            {                
                /* Set picture dimensions and clear buffer */
                SetBufferPicture( p_vout, p_pic );

                /* Render picture and informations */
                RenderPicture( p_vout, p_pic );             
                if( p_vout->b_info )
                {
                    RenderPictureInfo( p_vout, p_pic );
                    RenderInfo( p_vout );                
                }
            }
            
            /* Remove picture from heap */
            vlc_mutex_lock( &p_vout->picture_lock );
            p_pic->i_status = p_pic->i_refcount ? DISPLAYED_PICTURE : DESTROYED_PICTURE;
            vlc_mutex_unlock( &p_vout->picture_lock );                          

            /* Render interface and spus */
            if( b_display && p_vout->b_interface )
            {
                RenderInterface( p_vout );                
            }
            if( p_spu )
            {
                if( b_display )
                {                    
                    RenderSubPictureUnit( p_vout, p_spu );
                }                

                /* Remove spu from heap */
                vlc_mutex_lock( &p_vout->spu_lock );
                p_spu->i_status = DESTROYED_SPU;
                vlc_mutex_unlock( &p_vout->spu_lock );                          
            }

        }
        else if( p_spu )                                     /* spu alone */
        {
            b_display = p_vout->b_active;

            if( b_display )
            {                
                /* Clear buffer */
                SetBufferPicture( p_vout, NULL );

                /* Render informations, interface and spu */
                if( p_vout->b_info )
                {
                    RenderInfo( p_vout );
                }
                if( p_vout->b_interface )
                {
                    RenderInterface( p_vout );
                }
                RenderSubPictureUnit( p_vout, p_spu );            
            }            

            /* Remove spu from heap */
            vlc_mutex_lock( &p_vout->spu_lock );
            p_spu->i_status = DESTROYED_SPU;
            vlc_mutex_unlock( &p_vout->spu_lock );                          
        }
        else                                              /* idle screen alone */
        {            
            //??? render on idle screen or interface change
            b_display = 0;             //???
        }

        /*
         * Sleep, wake up and display rendered picture
         */

#ifdef STATS
        /* Store render time */
        p_vout->render_time = mdate() - current_date;
#endif

        /* Give back change lock */
        vlc_mutex_unlock( &p_vout->change_lock );        

        /* Sleep a while or until a given date */
        if( display_date != 0 )
        {
            mwait( display_date );
        }
        else
        {
            msleep( VOUT_IDLE_SLEEP );                
        }            

        /* On awakening, take back lock and send immediately picture to display, 
         * then swap buffers */
        vlc_mutex_lock( &p_vout->change_lock );        
#ifdef DEBUG_VIDEO
        intf_DbgMsg( "picture %p, spu %p\n", p_pic, p_spu );        
#endif            
        if( b_display && !(p_vout->i_changes & VOUT_NODISPLAY_CHANGE) )
        {
            vout_SysDisplay( p_vout );
            p_vout->i_buffer_index = ++p_vout->i_buffer_index & 1;
        }

        /*
         * Check events and manage thread
	 */
        if( vout_SysManage( p_vout ) | Manage( p_vout ) )
	{
	    /* A fatal error occured, and the thread must terminate immediately,
	     * without displaying anything - setting b_error to 1 cause the
	     * immediate end of the main while() loop. */
	    p_vout->b_error = 1;
	}  
    } 

    /*
     * Error loop - wait until the thread destruction is requested
     */
    if( p_vout->b_error )
    {
        ErrorThread( p_vout );        
    }

    /* End of thread */
    EndThread( p_vout );
    DestroyThread( p_vout, THREAD_OVER ); 
    intf_DbgMsg( "thread end\n" );
}

/******************************************************************************
 * ErrorThread: RunThread() error loop
 ******************************************************************************
 * This function is called when an error occured during thread main's loop. The
 * thread can still receive feed, but must be ready to terminate as soon as
 * possible.
 ******************************************************************************/
static void ErrorThread( vout_thread_t *p_vout )
{
    /* Wait until a `die' order */
    intf_DbgMsg("\n");
    while( !p_vout->b_die )
    {
        /* Sleep a while */
        msleep( VOUT_IDLE_SLEEP );                
    }
}

/*******************************************************************************
 * EndThread: thread destruction
 *******************************************************************************
 * This function is called when the thread ends after a sucessfull 
 * initialization. It frees all ressources allocated by InitThread.
 *******************************************************************************/
static void EndThread( vout_thread_t *p_vout )
{
    int     i_index;                                          /* index in heap */
            
    /* Store status */
    intf_DbgMsg("\n");
    *p_vout->pi_status = THREAD_END;    

    /* Destroy all remaining pictures and spus */
    for( i_index = 0; i_index < VOUT_MAX_PICTURES; i_index++ )
    {
	if( p_vout->p_picture[i_index].i_status != FREE_PICTURE )
	{
            free( p_vout->p_picture[i_index].p_data );
        }
        if( p_vout->p_spu[i_index].i_status != FREE_SPU )
        {
            free( p_vout->p_spu[i_index].p_data );            
        }        
    }

    /* Destroy translation tables */
    vout_EndTables( p_vout );  
    vout_SysEnd( p_vout );    
}

/*******************************************************************************
 * DestroyThread: thread destruction
 *******************************************************************************
 * This function is called when the thread ends. It frees all ressources
 * allocated by CreateThread. Status is available at this stage.
 *******************************************************************************/
static void DestroyThread( vout_thread_t *p_vout, int i_status )
{  
    int *pi_status;                                           /* status adress */
    
    /* Store status adress */
    intf_DbgMsg("\n");
    pi_status = p_vout->pi_status;    
    
    /* Destroy thread structures allocated by Create and InitThread */
    vout_UnloadFont( p_vout->p_default_font );
    vout_UnloadFont( p_vout->p_large_font ); 
    vout_SysDestroy( p_vout );
    free( p_vout );
    *pi_status = i_status;    
}

/*******************************************************************************
 * Print: print simple text on a picture
 *******************************************************************************
 * This function will print a simple text on the picture. It is designed to
 * print debugging or general informations.
 *******************************************************************************/
void Print( vout_thread_t *p_vout, int i_x, int i_y, int i_halign, int i_valign, unsigned char *psz_text )
{
    int                 i_text_height;                    /* total text height */
    int                 i_text_width;                      /* total text width */

    /* Update upper left coordinates according to alignment */
    vout_TextSize( p_vout->p_default_font, 0, psz_text, &i_text_width, &i_text_height );
    switch( i_halign )
    {
    case 0:                                                        /* centered */
        i_x -= i_text_width / 2;
        break;        
    case 1:                                                   /* right aligned */
        i_x -= i_text_width;
        break;                
    }
    switch( i_valign )
    {
    case 0:                                                        /* centered */
        i_y -= i_text_height / 2;
        break;        
    case 1:                                                   /* bottom aligned */
        i_y -= i_text_height;
        break;                
    }

    /* Check clipping */
    if( (i_y < 0) || (i_y + i_text_height > p_vout->i_height) || 
        (i_x < 0) || (i_x + i_text_width > p_vout->i_width) )
    {
        intf_DbgMsg("'%s' would print outside the screen\n", psz_text);        
        return;        
    }    

    /* Set area and print text */
    SetBufferArea( p_vout, i_x, i_y, i_text_width, i_text_height );    
    vout_Print( p_vout->p_default_font, p_vout->p_buffer[ p_vout->i_buffer_index ].p_data + 
                i_y * p_vout->i_bytes_per_line + i_x * p_vout->i_bytes_per_pixel,
                p_vout->i_bytes_per_pixel, p_vout->i_bytes_per_line, 
                0xffffffff, 0x00000000, 0x00000000, TRANSPARENT_TEXT, psz_text );
}

/*******************************************************************************
 * SetBufferArea: activate an area in current buffer
 *******************************************************************************
 * This function is called when something is rendered on the current buffer.
 * It set the area as active and prepare it to be cleared on next rendering.
 * Pay attention to the fact that in this functions, i_h is in fact the end y
 * coordinate of the new area.
 *******************************************************************************/
static void SetBufferArea( vout_thread_t *p_vout, int i_x, int i_y, int i_w, int i_h )
{
    vout_buffer_t *     p_buffer;                            /* current buffer */
    int                 i_area_begin, i_area_end;   /* area vertical extension */
    int                 i_area, i_area_copy;                     /* area index */    
    int                 i_area_shift;              /* shift distance for areas */    
    
    /* Choose buffer and modify h to end of area position */
    p_buffer =  &p_vout->p_buffer[ p_vout->i_buffer_index ];    
    i_h +=      i_y - 1;
 
    /* 
     * Remove part of the area which is inside the picture - this is done
     * by calling again SetBufferArea with the correct areas dimensions.
     */
    if( (i_x >= p_buffer->i_pic_x) && (i_x + i_w <= p_buffer->i_pic_x + p_buffer->i_pic_width) )
    {
        i_area_begin =  p_buffer->i_pic_y;
        i_area_end =    i_area_begin + p_buffer->i_pic_height - 1;

        if( ((i_y >= i_area_begin) && (i_y <= i_area_end)) ||
            ((i_h >= i_area_begin) && (i_h <= i_area_end)) ||
            ((i_y <  i_area_begin) && (i_h > i_area_end)) )
        {                    
            /* Keep the stripe above the picture, if any */
            if( i_y < i_area_begin )
            {
                SetBufferArea( p_vout, i_x, i_y, i_w, i_area_begin - i_y );            
            }
            /* Keep the stripe below the picture, if any */
            if( i_h > i_area_end )
            {
                SetBufferArea( p_vout, i_x, i_area_end, i_w, i_h - i_area_end );
            }        
            return;
        }        
    }   

    /* Skip some extensions until interesting areas */
    for( i_area = 0; 
         (i_area < p_buffer->i_areas) &&
             (p_buffer->pi_area_end[i_area] + 1 <= i_y); 
         i_area++ )
    {
        ;        
    }
    
    if( i_area == p_buffer->i_areas )
    {
        /* New area is below all existing ones: just add it at the end of the 
         * array, if possible - else, append it to the last one */
        if( i_area < VOUT_MAX_AREAS )
        {
            p_buffer->pi_area_begin[i_area] = i_y;
            p_buffer->pi_area_end[i_area] = i_h;            
            p_buffer->i_areas++;            
        }
        else
        {
#ifdef DEBUG_VIDEO
            intf_DbgMsg("areas overflow\n");            
#endif
            p_buffer->pi_area_end[VOUT_MAX_AREAS - 1] = i_h;  
        }        
    }
    else 
    {
        i_area_begin =  p_buffer->pi_area_begin[i_area];
        i_area_end =    p_buffer->pi_area_end[i_area];
        
        if( i_y < i_area_begin ) 
        {
            if( i_h >= i_area_begin - 1 )
            {                
                /* Extend area above */
                p_buffer->pi_area_begin[i_area] = i_y;
            }
            else
            {
                /* Create a new area above : merge last area if overflow, then 
                 * move all old areas down */
                if( p_buffer->i_areas == VOUT_MAX_AREAS )
                {                    
#ifdef DEBUG_VIDEO
                    intf_DbgMsg("areas overflow\n");       
#endif
                    p_buffer->pi_area_end[VOUT_MAX_AREAS - 2] = p_buffer->pi_area_end[VOUT_MAX_AREAS - 1];                    
                }
                else
                {
                    p_buffer->i_areas++;                    
                }
                for( i_area_copy = p_buffer->i_areas - 1; i_area_copy > i_area; i_area_copy++ )
                {
                    p_buffer->pi_area_begin[i_area_copy] = p_buffer->pi_area_begin[i_area_copy - 1];
                    p_buffer->pi_area_end[i_area_copy] =   p_buffer->pi_area_end[i_area_copy - 1];
                }
                p_buffer->pi_area_begin[i_area] = i_y;
                p_buffer->pi_area_end[i_area] = i_h;
                return;
            }              
        }
        if( i_h > i_area_end )
        {
            /* Find further areas which can be merged with the new one */
            for( i_area_copy = i_area + 1; 
                 (i_area_copy < p_buffer->i_areas) &&
                     (p_buffer->pi_area_begin[i_area] <= i_h);
                 i_area_copy++ )
            {
                ;                
            }
            i_area_copy--;            

            if( i_area_copy != i_area )
            {
                /* Merge with last possible areas */
                p_buffer->pi_area_end[i_area] = MAX( i_h, p_buffer->pi_area_end[i_area_copy] );

                /* Shift lower areas upward */
                i_area_shift = i_area_copy - i_area;                
                p_buffer->i_areas -= i_area_shift;
                for( i_area_copy = i_area + 1; i_area_copy < p_buffer->i_areas; i_area_copy++ )
                {
                    p_buffer->pi_area_begin[i_area_copy] = p_buffer->pi_area_begin[i_area_copy + i_area_shift];
                    p_buffer->pi_area_end[i_area_copy] =   p_buffer->pi_area_end[i_area_copy + i_area_shift];
                }
            }
            else
            {
                /* Extend area below */
                p_buffer->pi_area_end[i_area] = i_h;
            }            
        }
    }
}

/*******************************************************************************
 * SetBufferPicture: clear buffer and set picture area
 *******************************************************************************
 * This function is called before any rendering. It clears the current 
 * rendering buffer and set the new picture area. If the picture pointer is
 * NULL, then no picture area is defined. Floating operations are avoided since
 * some MMX calculations may follow.
 *******************************************************************************/
static void SetBufferPicture( vout_thread_t *p_vout, picture_t *p_pic )
{
    vout_buffer_t *     p_buffer;                            /* current buffer */
    int                 i_pic_x, i_pic_y;                  /* picture position */
    int                 i_pic_width, i_pic_height;       /* picture dimensions */    
    int                 i_old_pic_y, i_old_pic_height;     /* old picture area */    
    int                 i_vout_width, i_vout_height;     /* display dimensions */
    int                 i_area;                                  /* area index */    
    int                 i_data_index;                       /* area data index */    
    int                 i_data_size;     /* area data size, in 256 bytes blocs */
    u64 *               p_data;                                   /* area data */    
    
    /* Choose buffer and set display dimensions */
    p_buffer =          &p_vout->p_buffer[ p_vout->i_buffer_index ];    
    i_vout_width =      p_vout->i_width;
    i_vout_height =     p_vout->i_height;    

    /*
     * Computes new picture size 
     */
    if( p_pic != NULL )
    {
        /* Try horizontal scaling first */
        i_pic_width = ( p_vout->b_scale || (p_pic->i_width > i_vout_width)) ? 
            i_vout_width : p_pic->i_width;
        i_pic_width = i_pic_width / 16 * 16; //?? currently, width must be multiple of 16        
        switch( p_pic->i_aspect_ratio )
        {
        case AR_3_4_PICTURE:
            i_pic_height = i_pic_width * 3 / 4;
            break;                
        case AR_16_9_PICTURE:
            i_pic_height = i_pic_width * 9 / 16;
            break;
        case AR_221_1_PICTURE:        
            i_pic_height = i_pic_width * 100 / 221;
            break;               
        case AR_SQUARE_PICTURE:
        default:
            i_pic_height = p_pic->i_height * i_pic_width / p_pic->i_width;            
            break;
        }

        /* If picture dimensions using horizontal scaling are too large, use 
         * vertical scaling */
        if( i_pic_height > i_vout_height )
        {
            i_pic_height = ( p_vout->b_scale || (p_pic->i_height > i_vout_height)) ? 
                i_vout_height : p_pic->i_height;
            switch( p_pic->i_aspect_ratio )
            {
            case AR_3_4_PICTURE:
                i_pic_width = i_pic_height * 4 / 3;
                break;                
            case AR_16_9_PICTURE:
                i_pic_width = i_pic_height * 16 / 9;
                break;
            case AR_221_1_PICTURE:        
                i_pic_width = i_pic_height * 221 / 100;
                break;               
            case AR_SQUARE_PICTURE:
            default:
                i_pic_width = p_pic->i_width * i_pic_height / p_pic->i_height;
                break;
            }        
            i_pic_width = i_pic_width / 16 * 16; //?? currently, width must be multiple of 16        
        }        

        /* Set picture position */
        i_pic_x = (p_vout->i_width - i_pic_width) / 2;
        i_pic_y = (p_vout->i_height - i_pic_height) / 2;                
    }    
    else
    {
        /* No picture: size is 0 */
        i_pic_x =       0;
        i_pic_y =       0;
        i_pic_width =   0;
        i_pic_height =  0;
    }

    /*
     * Set new picture size - if is is smaller than the previous one, clear 
     * around it. Since picture are centered, only their size is tested.
     */                                          
    if( (p_buffer->i_pic_width > i_pic_width) || (p_buffer->i_pic_height > i_pic_height) )
    {
        i_old_pic_y =            p_buffer->i_pic_y;
        i_old_pic_height =       p_buffer->i_pic_height;
        p_buffer->i_pic_x =      i_pic_x;
        p_buffer->i_pic_y =      i_pic_y;
        p_buffer->i_pic_width =  i_pic_width;
        p_buffer->i_pic_height = i_pic_height;                        
        SetBufferArea( p_vout, 0, i_old_pic_y, p_vout->i_width, i_old_pic_height );
    }
    else
    {
        p_buffer->i_pic_x =      i_pic_x;
        p_buffer->i_pic_y =      i_pic_y;
        p_buffer->i_pic_width =  i_pic_width;
        p_buffer->i_pic_height = i_pic_height;    
    }

    /*
     * Clear areas
     */
    for( i_area = 0; i_area < p_buffer->i_areas; i_area++ )
    {
#ifdef DEBUG_VIDEO    
        intf_DbgMsg("clearing picture %p area: %d-%d\n", p_pic, 
                    p_buffer->pi_area_begin[i_area], p_buffer->pi_area_end[i_area]);    
#endif
        p_data = (u64*) (p_buffer->p_data + p_vout->i_bytes_per_line * p_buffer->pi_area_begin[i_area]);
        i_data_size = (p_buffer->pi_area_end[i_area] - p_buffer->pi_area_begin[i_area] + 1) * 
            p_vout->i_bytes_per_line / 256;
        for( i_data_index = 0; i_data_index < i_data_size; i_data_index++ )
        {
            /* Clear 256 bytes block */
            *p_data++ = 0;  *p_data++ = 0;  *p_data++ = 0;  *p_data++ = 0;
            *p_data++ = 0;  *p_data++ = 0;  *p_data++ = 0;  *p_data++ = 0;
            *p_data++ = 0;  *p_data++ = 0;  *p_data++ = 0;  *p_data++ = 0;
            *p_data++ = 0;  *p_data++ = 0;  *p_data++ = 0;  *p_data++ = 0;
            *p_data++ = 0;  *p_data++ = 0;  *p_data++ = 0;  *p_data++ = 0;
            *p_data++ = 0;  *p_data++ = 0;  *p_data++ = 0;  *p_data++ = 0;
            *p_data++ = 0;  *p_data++ = 0;  *p_data++ = 0;  *p_data++ = 0;
            *p_data++ = 0;  *p_data++ = 0;  *p_data++ = 0;  *p_data++ = 0;
        }
        i_data_size = (p_buffer->pi_area_end[i_area] - p_buffer->pi_area_begin[i_area] + 1) *
            p_vout->i_bytes_per_line % 256 / 4;
        for( i_data_index = 0; i_data_index < i_data_size; i_data_index++ )
        {
            /* Clear remaining 4 bytes blocks */
            *p_data++ = 0;
        }
    }    

    /*
     * Clear areas array
     */
    p_buffer->i_areas = 0;
}

/******************************************************************************
 * RenderPicture: render a picture
 ******************************************************************************
 * This function convert a picture from a video heap to a pixel-encoded image
 * and copy it to the current rendering buffer. No lock is required, since the
 * rendered picture has been determined as existant, and will only be destroyed
 * by the vout thread later.
 ******************************************************************************/
static void RenderPicture( vout_thread_t *p_vout, picture_t *p_pic )
{
    vout_buffer_t *     p_buffer;                         /* rendering buffer */    
    byte_t *            p_convert_dst;              /* convertion destination */
    int                 i_width, i_height, i_eol, i_pic_eol, i_scale;        /* ?? tmp variables*/
    
    /* Get and set rendering informations */
    p_buffer = &p_vout->p_buffer[ p_vout->i_buffer_index ];    
    p_vout->last_picture_date = p_pic->date;
    p_convert_dst = p_buffer->p_data + p_buffer->i_pic_x * p_vout->i_bytes_per_pixel +
        p_buffer->i_pic_y * p_vout->i_bytes_per_line;

    // ?? temporary section: rebuild aspect scale from size informations.
    // ?? when definitive convertion prototype will be used, those info will
    // ?? no longer be required
    i_width = MIN( p_pic->i_width, p_buffer->i_pic_width );
    i_eol = p_pic->i_width - i_width / 16 * 16;
    i_pic_eol = p_vout->i_bytes_per_line / p_vout->i_bytes_per_pixel - i_width;    
    if( p_pic->i_height == p_buffer->i_pic_height )
    {        
        i_scale = 0;    
    }    
    else
    {
        i_scale = p_pic->i_height / (p_pic->i_height - p_buffer->i_pic_height);        
    }    
    i_eol = p_pic->i_width - p_buffer->i_pic_width;    
    i_height = p_pic->i_height * i_width / p_pic->i_width;
    // ?? end of temporary code

    /*
     * Choose appropriate rendering function and render picture
     */
    switch( p_pic->i_type )
    {
    case YUV_420_PICTURE:
        p_vout->p_ConvertYUV420( p_vout, p_convert_dst, 
                                 p_pic->p_y, p_pic->p_u, p_pic->p_v,
                                 i_width, i_height, i_eol, i_pic_eol, i_scale, 
                                 p_pic->i_matrix_coefficients );
        break;        
    case YUV_422_PICTURE:
        p_vout->p_ConvertYUV422( p_vout, p_convert_dst, 
                                 p_pic->p_y, p_pic->p_u, p_pic->p_v,
                                 i_width, i_height, i_eol, i_pic_eol, i_scale, 
                                 p_pic->i_matrix_coefficients );
        break;        
    case YUV_444_PICTURE:
        p_vout->p_ConvertYUV444( p_vout, p_convert_dst, 
                                 p_pic->p_y, p_pic->p_u, p_pic->p_v,
                                 i_width, i_height, i_eol, i_pic_eol, i_scale, 
                                 p_pic->i_matrix_coefficients );
        break;                
#ifdef DEBUG
    default:        
        intf_DbgMsg("error: unknown picture type %d\n", p_pic->i_type );
        break;        
#endif
    }
}

/******************************************************************************
 * RenderPictureInfo: print additionnal informations on a picture
 ******************************************************************************
 * This function will print informations such as fps and other picture
 * dependant informations.
 ******************************************************************************/
static void RenderPictureInfo( vout_thread_t *p_vout, picture_t *p_pic )
{
#if defined(STATS) || defined(DEBUG)
    char        psz_buffer[256];                             /* string buffer */
#endif

#ifdef STATS
    /* 
     * Print FPS rate in upper right corner 
     */
    if( p_vout->c_fps_samples > VOUT_FPS_SAMPLES )
    {        
        sprintf( psz_buffer, "%.2f fps", (double) VOUT_FPS_SAMPLES * 1000000 /
                 ( p_vout->p_fps_sample[ (p_vout->c_fps_samples - 1) % VOUT_FPS_SAMPLES ] -
                   p_vout->p_fps_sample[ p_vout->c_fps_samples % VOUT_FPS_SAMPLES ] ) );        
        Print( p_vout, p_vout->i_width, 0, 1, -1, psz_buffer );
    }

    /* 
     * Print frames count and loop time in upper left corner 
     */
    sprintf( psz_buffer, "%ld frames   rendering: %ld us", 
             (long) p_vout->c_fps_samples, (long) p_vout->render_time );
    Print( p_vout, 0, 0, -1, -1, psz_buffer );
#endif

#ifdef DEBUG
    /*
     * Print picture information in lower right corner
     */
    sprintf( psz_buffer, "%s picture %dx%d (%dx%d%+d%+d %s) -> %dx%d+%d+%d",
             (p_pic->i_type == YUV_420_PICTURE) ? "4:2:0" :
             ((p_pic->i_type == YUV_422_PICTURE) ? "4:2:2" :
              ((p_pic->i_type == YUV_444_PICTURE) ? "4:4:4" : "ukn-type")),
             p_pic->i_width, p_pic->i_height,
             p_pic->i_display_width, p_pic->i_display_height,
             p_pic->i_display_horizontal_offset, p_pic->i_display_vertical_offset,
             (p_pic->i_aspect_ratio == AR_SQUARE_PICTURE) ? "sq" :
             ((p_pic->i_aspect_ratio == AR_3_4_PICTURE) ? "4:3" :
              ((p_pic->i_aspect_ratio == AR_16_9_PICTURE) ? "16:9" :
               ((p_pic->i_aspect_ratio == AR_221_1_PICTURE) ? "2.21:1" : "ukn-ar" ))),
             p_vout->p_buffer[ p_vout->i_buffer_index ].i_pic_width,
             p_vout->p_buffer[ p_vout->i_buffer_index ].i_pic_height,
             p_vout->p_buffer[ p_vout->i_buffer_index ].i_pic_x,
             p_vout->p_buffer[ p_vout->i_buffer_index ].i_pic_y );    
    Print( p_vout, p_vout->i_width, p_vout->i_height, 1, 1, psz_buffer );
#endif
}

/******************************************************************************
 * RenderIdle: render idle picture
 ******************************************************************************
 * This function will print something on the screen.
 ******************************************************************************/
static void RenderIdle( vout_thread_t *p_vout )
{
    //??
    Print( p_vout, p_vout->i_width / 2, p_vout->i_height / 2, 0, 0, 
           "no stream" );        //??
}

/******************************************************************************
 * RenderInfo: render additionnal informations
 ******************************************************************************
 * This function render informations which do not depend of the current picture
 * rendered.
 ******************************************************************************/
static void RenderInfo( vout_thread_t *p_vout )
{
#ifdef DEBUG
    char        psz_buffer[256];                             /* string buffer */
    int         i_ready_pic = 0;                            /* ready pictures */
    int         i_reserved_pic = 0;                      /* reserved pictures */
    int         i_picture;                                   /* picture index */
#endif

#ifdef DEBUG
    /* 
     * Print thread state in lower left corner  
     */
    for( i_picture = 0; i_picture < VOUT_MAX_PICTURES; i_picture++ )
    {
        switch( p_vout->p_picture[i_picture].i_status )
        {
        case RESERVED_PICTURE:
        case RESERVED_DATED_PICTURE:
        case RESERVED_DISP_PICTURE:
            i_reserved_pic++;            
            break;            
        case READY_PICTURE:
            i_ready_pic++;            
            break;            
        }        
    }
    sprintf( psz_buffer, "pic: %d/%d/%d", 
             i_reserved_pic, i_ready_pic, VOUT_MAX_PICTURES );
    Print( p_vout, 0, p_vout->i_height, -1, 1, psz_buffer );    
#endif
}

/*******************************************************************************
 * RenderSubPictureUnit: render an spu
 *******************************************************************************
 * This function render a sub picture unit.
 *******************************************************************************/
static void RenderSubPictureUnit( vout_thread_t *p_vout, spu_t *p_spu )
{
    //??
}

/*******************************************************************************
 * RenderInterface: render the interface
 *******************************************************************************
 * This function render the interface, if any.
 * ?? this is obviously only a temporary interface !
 *******************************************************************************/
static void RenderInterface( vout_thread_t *p_vout )
{
    int         i_height, i_text_height;              /* total and text height */
    int         i_width_1, i_width_2;                            /* text width */
    int         i_byte;                                          /* byte index */    
    const char *psz_text_1 = "[1-9] Channel   [i]nfo   [c]olor     [g/G]amma";
    const char *psz_text_2 = "[+/-] Volume    [m]ute   [s]caling   [Q]uit";    

    /* Get text size */
    vout_TextSize( p_vout->p_large_font, OUTLINED_TEXT | TRANSPARENT_TEXT, psz_text_1, &i_width_1, &i_height );
    vout_TextSize( p_vout->p_large_font, OUTLINED_TEXT | TRANSPARENT_TEXT, psz_text_2, &i_width_2, &i_text_height );
    i_height += i_text_height;

    /* Render background - effective background color will depend of the screen
     * depth */
    for( i_byte = (p_vout->i_height - i_height) * p_vout->i_bytes_per_line;
         i_byte < p_vout->i_height * p_vout->i_bytes_per_line;
         i_byte++ )
    {
        p_vout->p_buffer[ p_vout->i_buffer_index ].p_data[ i_byte ] = 0x33;        
    }    

    /* Render text, if not larger than screen */
    if( i_width_1 < p_vout->i_width )
    {        
        vout_Print( p_vout->p_large_font, p_vout->p_buffer[ p_vout->i_buffer_index ].p_data +
                    (p_vout->i_height - i_height) * p_vout->i_bytes_per_line,
                    p_vout->i_bytes_per_pixel, p_vout->i_bytes_per_line,
                    0xffffffff, 0x00000000, 0x00000000,
                    OUTLINED_TEXT | TRANSPARENT_TEXT, psz_text_1 );
    }
    if( i_width_2 < p_vout->i_width )
    {        
        vout_Print( p_vout->p_large_font, p_vout->p_buffer[ p_vout->i_buffer_index ].p_data +
                    (p_vout->i_height - i_height + i_text_height) * p_vout->i_bytes_per_line,
                    p_vout->i_bytes_per_pixel, p_vout->i_bytes_per_line,
                    0xffffffff, 0x00000000, 0x00000000,
                    OUTLINED_TEXT | TRANSPARENT_TEXT, psz_text_2 );
    }    

    /* Activate modified area */
    SetBufferArea( p_vout, 0, p_vout->i_height - i_height, p_vout->i_width, i_height );
}

/******************************************************************************
 * Manage: manage thread
 ******************************************************************************
 * This function will handle changes in thread configuration.
 ******************************************************************************/
static int Manage( vout_thread_t *p_vout )
{
#ifdef DEBUG_VIDEO
    if( p_vout->i_changes )
    {
        intf_DbgMsg("changes: 0x%x (no display: 0x%x)\n", p_vout->i_changes, 
                    p_vout->i_changes & VOUT_NODISPLAY_CHANGE );        
    }    
#endif

    /* On gamma or grayscale change, rebuild tables */
    if( p_vout->i_changes & (VOUT_GAMMA_CHANGE | VOUT_GRAYSCALE_CHANGE) )
    {
        vout_ResetTables( p_vout );        
    }    

    /* Clear changes flags which does not need management or have been handled */
    p_vout->i_changes &= ~(VOUT_GAMMA_CHANGE | VOUT_GRAYSCALE_CHANGE |
                           VOUT_INFO_CHANGE | VOUT_INTF_CHANGE | VOUT_SCALE_CHANGE );

    /* Detect unauthorized changes */
    if( p_vout->i_changes )
    {
        /* Some changes were not acknowledged by vout_SysManage or this function,
         * it means they should not be authorized */
        intf_ErrMsg( "error: unauthorized changes in the video output thread\n" );        
        return( 1 );        
    }
    
    return( 0 );    
}
