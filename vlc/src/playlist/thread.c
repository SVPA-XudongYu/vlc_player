/*****************************************************************************
 * thread.c : Playlist management functions
 *****************************************************************************
 * Copyright © 1999-2008 VLC authors and VideoLAN
 * $Id$
 *
 * Authors: Samuel Hocevar <sam@zoy.org>
 *          Clément Stenac <zorglub@videolan.org>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/
#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <assert.h>

#include <stdio.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <netdb.h>

#include <vlc_common.h>
#include <vlc_es.h>
#include <vlc_input.h>
#include <vlc_interface.h>
#include <vlc_playlist.h>
#include <vlc_rand.h>
#include "playlist_internal.h"

/*****************************************************************************
 * Local prototypes
 *****************************************************************************/
static void *Thread   ( void * );

/*****************************************************************************
 * Main functions for the global thread
 *****************************************************************************/


const char PLAYER_STATUS_EVENT_RECEIVER[] = "127.0.0.1";
const int PLAYER_STATUS_EVENT_RECEIVER_PORT = 5000;
const char END_OF_STREAM[] = "EndOfStream";
int send_sre_event(const char ip[], const int port, const char *message);



/**
 * Creates the main playlist thread.
 */
void playlist_Activate( playlist_t *p_playlist )
{
    playlist_private_t *p_sys = pl_priv(p_playlist);

    if( vlc_clone( &p_sys->thread, Thread, p_playlist,
                   VLC_THREAD_PRIORITY_LOW ) )
    {
        msg_Err( p_playlist, "cannot spawn playlist thread" );
        abort();
    }
}

/**
 * Stops the playlist forever (but do not destroy it yet).
 * Any input is stopped.
 * \return Nothing but waits for the playlist to be deactivated.
 */
void playlist_Deactivate( playlist_t *p_playlist )
{
    playlist_private_t *p_sys = pl_priv(p_playlist);

    PL_LOCK;
    /* WARNING: There is a latent bug. It is assumed that only one thread will
     * be waiting for playlist deactivation at a time. So far, that works
     * as playlist_Deactivate() is only ever called while closing an
     * interface and interfaces are shut down serially by intf_DestroyAll(). */
    if( p_sys->killed )
    {
        PL_UNLOCK;
        return;
    }

    msg_Dbg( p_playlist, "deactivating the playlist" );
    p_sys->killed = true;
    vlc_cond_signal( &p_sys->signal );
    PL_UNLOCK;

    vlc_join( p_sys->thread, NULL );
}

/* */

/* Input Callback */
static int InputEvent( vlc_object_t *p_this, char const *psz_cmd,
                       vlc_value_t oldval, vlc_value_t newval, void *p_data )
{
    VLC_UNUSED(p_this); VLC_UNUSED(psz_cmd); VLC_UNUSED(oldval);
    playlist_t *p_playlist = p_data;

    if( newval.i_int != INPUT_EVENT_STATE &&
        newval.i_int != INPUT_EVENT_DEAD )
        return VLC_SUCCESS;

    PL_LOCK;

    /* XXX: signaling while not changing any parameter... suspicious... */
    vlc_cond_signal( &pl_priv(p_playlist)->signal );

    PL_UNLOCK;
    return VLC_SUCCESS;
}

/**
 * Synchronise the current index of the playlist
 * to match the index of the current item.
 *
 * \param p_playlist the playlist structure
 * \param p_cur the current playlist item
 * \return nothing
 */
void ResyncCurrentIndex( playlist_t *p_playlist, playlist_item_t *p_cur )
{
    PL_ASSERT_LOCKED;

    PL_DEBUG( "resyncing on %s", PLI_NAME( p_cur ) );
    /* Simply resync index */
    int i;
    p_playlist->i_current_index = -1;
    for( i = 0 ; i< p_playlist->current.i_size; i++ )
    {
        if( ARRAY_VAL( p_playlist->current, i ) == p_cur )
        {
            p_playlist->i_current_index = i;
            break;
        }
    }
    PL_DEBUG( "%s is at %i", PLI_NAME( p_cur ), p_playlist->i_current_index );
}

/**
 * Reset the currently playing playlist.
 *
 * \param p_playlist the playlist structure
 * \param p_cur the current playlist item
 * \return nothing
 */
void ResetCurrentlyPlaying( playlist_t *p_playlist,
                                   playlist_item_t *p_cur )
{
    playlist_private_t *p_sys = pl_priv(p_playlist);

    PL_DEBUG( "rebuilding array of current - root %s",
              PLI_NAME( p_sys->status.p_node ) );
    ARRAY_RESET( p_playlist->current );
    p_playlist->i_current_index = -1;
    for( playlist_item_t *p_next = NULL; ; )
    {
        /** FIXME: this is *slow* */
        p_next = playlist_GetNextLeaf( p_playlist,
                                       p_sys->status.p_node,
                                       p_next, true, false );
        if( !p_next )
            break;

        if( p_next == p_cur )
            p_playlist->i_current_index = p_playlist->current.i_size;
        ARRAY_APPEND( p_playlist->current, p_next);
    }
    PL_DEBUG("rebuild done - %i items, index %i", p_playlist->current.i_size,
                                                  p_playlist->i_current_index);

    if( var_GetBool( p_playlist, "random" ) && ( p_playlist->current.i_size > 0 ) )
    {
        /* Shuffle the array */
        for( unsigned j = p_playlist->current.i_size - 1; j > 0; j-- )
        {
            unsigned i = vlc_lrand48() % (j+1); /* between 0 and j */
            playlist_item_t *p_tmp;
            /* swap the two items */
            p_tmp = ARRAY_VAL(p_playlist->current, i);
            ARRAY_VAL(p_playlist->current,i) = ARRAY_VAL(p_playlist->current,j);
            ARRAY_VAL(p_playlist->current,j) = p_tmp;
        }
    }
    p_sys->b_reset_currently_playing = false;
}


/**
 * Start the input for an item
 *
 * \param p_playlist the playlist object
 * \param p_item the item to play
 */
static bool PlayItem( playlist_t *p_playlist, playlist_item_t *p_item )
{
    playlist_private_t *p_sys = pl_priv(p_playlist);
    input_item_t *p_input = p_item->p_input;

    PL_ASSERT_LOCKED;

    msg_Dbg( p_playlist, "creating new input thread" );

    p_item->i_nb_played++;
    set_current_status_item( p_playlist, p_item );
    assert( p_sys->p_input == NULL );
    PL_UNLOCK;

    input_thread_t *p_input_thread = input_Create( p_playlist, p_input, NULL,
                                                   p_sys->p_input_resource );
    if( likely(p_input_thread != NULL) )
    {
        var_AddCallback( p_input_thread, "intf-event",
                         InputEvent, p_playlist );

        if( input_Start( p_input_thread ) )
        {
            var_DelCallback( p_input_thread, "intf-event",
                             InputEvent, p_playlist );
            vlc_object_release( p_input_thread );
            p_input_thread = NULL;
        }
    }

    /* TODO store art policy in playlist private data */
    char *psz_arturl = input_item_GetArtURL( p_input );
    /* p_input->p_meta should not be null after a successful CreateThread */
    bool b_has_art = !EMPTY_STR( psz_arturl );

    if( !b_has_art || strncmp( psz_arturl, "attachment://", 13 ) )
    {
        PL_DEBUG( "requesting art for new input thread" );
        libvlc_ArtRequest( p_playlist->obj.libvlc, p_input, META_REQUEST_OPTION_NONE );
    }
    free( psz_arturl );

    PL_LOCK;
    p_sys->p_input = p_input_thread;
    PL_UNLOCK;

    var_SetAddress( p_playlist, "input-current", p_input_thread );

    PL_LOCK;
    return p_input_thread != NULL;
}

/**
 * Compute the next playlist item depending on
 * the playlist course mode (forward, backward, random, view,...).
 *
 * \param p_playlist the playlist object
 * \return nothing
 */
static playlist_item_t *NextItem( playlist_t *p_playlist )
{
    playlist_private_t *p_sys = pl_priv(p_playlist);
    playlist_item_t *p_new = NULL;
    bool requested = p_sys->request.b_request;

    /* Clear the request */
    p_sys->request.b_request = false;

    /* Handle quickly a few special cases */
    /* No items to play */
    if( p_playlist->items.i_size == 0 )
    {
        msg_Info( p_playlist, "playlist is empty" );
        return NULL;
    }

    /* Start the real work */
    if( requested )
    {
        p_new = p_sys->request.p_item;

        if( p_new == NULL && p_sys->request.p_node == NULL )
            return NULL; /* Stop request! */

        int i_skip = p_sys->request.i_skip;
        PL_DEBUG( "processing request item: %s, node: %s, skip: %i",
                        PLI_NAME( p_sys->request.p_item ),
                        PLI_NAME( p_sys->request.p_node ), i_skip );

        if( p_sys->request.p_node &&
            p_sys->request.p_node != get_current_status_node( p_playlist ) )
        {

            set_current_status_node( p_playlist, p_sys->request.p_node );
            p_sys->request.p_node = NULL;
            p_sys->b_reset_currently_playing = true;
        }

        /* If we are asked for a node, go to it's first child */
        if( i_skip == 0 && ( p_new == NULL || p_new->i_children != -1 ) )
        {
            i_skip++;
            if( p_new != NULL )
            {
                p_new = playlist_GetNextLeaf( p_playlist, p_new, NULL, true, false );
                for( int i = 0; i < p_playlist->current.i_size; i++ )
                {
                    if( p_new == ARRAY_VAL( p_playlist->current, i ) )
                    {
                        p_playlist->i_current_index = i;
                        i_skip = 0;
                    }
                }
            }
        }

        if( p_sys->b_reset_currently_playing )
            /* A bit too bad to reset twice ... */
            ResetCurrentlyPlaying( p_playlist, p_new );
        else if( p_new )
            ResyncCurrentIndex( p_playlist, p_new );
        else
            p_playlist->i_current_index = -1;

        if( p_playlist->current.i_size && (i_skip > 0) )
        {
            if( p_playlist->i_current_index < -1 )
                p_playlist->i_current_index = -1;
            for( int i = i_skip; i > 0 ; i-- )
            {
                p_playlist->i_current_index++;
                if( p_playlist->i_current_index >= p_playlist->current.i_size )
                {
                    PL_DEBUG( "looping - restarting at beginning of node" );
                    /* reshuffle playlist when end is reached */
                    if( var_GetBool( p_playlist, "random" ) ) {
                        PL_DEBUG( "reshuffle playlist" );
                        ResetCurrentlyPlaying( p_playlist,
                                get_current_status_item( p_playlist ) );
                    }
                    p_playlist->i_current_index = 0;
                }
            }
            p_new = ARRAY_VAL( p_playlist->current,
                               p_playlist->i_current_index );
        }
        else if( p_playlist->current.i_size && (i_skip < 0) )
        {
            for( int i = i_skip; i < 0 ; i++ )
            {
                p_playlist->i_current_index--;
                if( p_playlist->i_current_index <= -1 )
                {
                    PL_DEBUG( "looping - restarting at end of node" );
                    /* reshuffle playlist when beginning is reached */
                    if( var_GetBool( p_playlist, "random" ) ) {
                        PL_DEBUG( "reshuffle playlist" );
                        ResetCurrentlyPlaying( p_playlist,
                                get_current_status_item( p_playlist ) );
                    }
                    p_playlist->i_current_index = p_playlist->current.i_size-1;
                }
            }
            p_new = ARRAY_VAL( p_playlist->current,
                               p_playlist->i_current_index );
        }
    }
    /* "Automatic" item change ( next ) */
    else
    {
        bool b_loop = var_GetBool( p_playlist, "loop" );
        bool b_repeat = var_GetBool( p_playlist, "repeat" );
        bool b_playstop = var_InheritBool( p_playlist, "play-and-stop" );

        /* Repeat and play/stop */
        if( b_repeat && get_current_status_item( p_playlist ) )
        {
            msg_Dbg( p_playlist,"repeating item" );
            return get_current_status_item( p_playlist );
        }
        if( b_playstop )
        {
            msg_Dbg( p_playlist,"stopping (play and stop)" );
            return NULL;
        }

        /* */
        if( get_current_status_item( p_playlist ) )
        {
            playlist_item_t *p_parent = get_current_status_item( p_playlist );
            while( p_parent )
            {
                if( p_parent->i_flags & PLAYLIST_SKIP_FLAG )
                {
                    msg_Dbg( p_playlist, "blocking item, stopping") ;
                    return NULL;
                }
                p_parent = p_parent->p_parent;
            }
        }

        PL_DEBUG( "changing item without a request (current %i/%i)",
                  p_playlist->i_current_index, p_playlist->current.i_size );
        /* Can't go to next from current item */
        if( get_current_status_item( p_playlist ) &&
            get_current_status_item( p_playlist )->i_flags & PLAYLIST_SKIP_FLAG )
            return NULL;

        if( p_sys->b_reset_currently_playing )
            ResetCurrentlyPlaying( p_playlist,
                                   get_current_status_item( p_playlist ) );

        p_playlist->i_current_index++;
        assert( p_playlist->i_current_index <= p_playlist->current.i_size );
        if( p_playlist->i_current_index == p_playlist->current.i_size )
        {
            if( !b_loop || p_playlist->current.i_size == 0 )
                return NULL;
            /* reshuffle after last item has been played */
            if( var_GetBool( p_playlist, "random" ) ) {
                PL_DEBUG( "reshuffle playlist" );
                ResetCurrentlyPlaying( p_playlist,
                                       get_current_status_item( p_playlist ) );
            }
            p_playlist->i_current_index = 0;
        }
        PL_DEBUG( "using item %i", p_playlist->i_current_index );
        if ( p_playlist->current.i_size == 0 )
            return NULL;

        p_new = ARRAY_VAL( p_playlist->current, p_playlist->i_current_index );
        /* The new item can't be autoselected  */
        if( p_new != NULL && p_new->i_flags & PLAYLIST_SKIP_FLAG )
            return NULL;
    }
    return p_new;
}

static void LoopInput( playlist_t *p_playlist )
{
    int playback_forced_stop = 0;
    playlist_private_t *p_sys = pl_priv(p_playlist);
    input_thread_t *p_input = p_sys->p_input;

    assert( p_input != NULL );

    if( p_sys->request.b_request || p_sys->killed )
    {
        PL_DEBUG( "incoming request - stopping current input" );
        playback_forced_stop = 1;
        input_Stop( p_input );
    }

    switch( var_GetInteger( p_input, "state" ) )
    {
    case END_S:
    case ERROR_S:
    /* This input is dead. Remove it ! */
        p_sys->p_input = NULL;
        PL_DEBUG( "dead input" );
        PL_UNLOCK;

        var_SetAddress( p_playlist, "input-current", NULL );

        /* WARNING: Input resource manipulation and callback deletion are
         * incompatible with the playlist lock. */
        if( !var_InheritBool( p_input, "sout-keep" ) )
            input_resource_TerminateSout( p_sys->p_input_resource );
        var_DelCallback( p_input, "intf-event", InputEvent, p_playlist );

        input_Close( p_input );
        PL_LOCK;
        //YUX: send EOS event
        if (playback_forced_stop == 0) {
	    send_sre_event(PLAYER_STATUS_EVENT_RECEIVER, PLAYER_STATUS_EVENT_RECEIVER_PORT, END_OF_STREAM);
        }
        break;
    default:
        vlc_cond_wait( &p_sys->signal, &p_sys->lock );
    }
}

static bool Next( playlist_t *p_playlist )
{
    playlist_item_t *p_item = NextItem( p_playlist );
    if( p_item == NULL )
        return false;

    msg_Dbg( p_playlist, "starting playback of new item" );
    ResyncCurrentIndex( p_playlist, p_item );
    return PlayItem( p_playlist, p_item );
}

/**
 * Run the main control thread itself
 */
static void *Thread ( void *data )
{
    playlist_t *p_playlist = data;
    playlist_private_t *p_sys = pl_priv(p_playlist);

    PL_LOCK;
    while( !p_sys->killed )
    {
        /* Playlist in stopped state */
        assert(p_sys->p_input == NULL);

        if( !p_sys->request.b_request )
        {
            vlc_cond_wait( &p_sys->signal, &p_sys->lock );
            continue;
        }

        while( !p_sys->killed && Next( p_playlist ) )
        {   /* Playlist in running state */
            assert(p_sys->p_input != NULL);

            do
                LoopInput( p_playlist );
            while( p_sys->p_input != NULL );
        }

        msg_Dbg( p_playlist, "nothing to play" );
        if( var_InheritBool( p_playlist, "play-and-exit" ) )
        {
            msg_Info( p_playlist, "end of playlist, exiting" );
            libvlc_Quit( p_playlist->obj.libvlc );
        }

        /* Destroy any video display now (XXX: ugly hack) */
        if( input_resource_HasVout( p_sys->p_input_resource ) )
        {
            PL_UNLOCK; /* Mind: NO LOCKS while manipulating input resources! */
            input_resource_TerminateVout( p_sys->p_input_resource );
            PL_LOCK;
        }
    }
    PL_UNLOCK;

    input_resource_Terminate( p_sys->p_input_resource );
    return NULL;
}



int send_sre_event(const char ip[], const int port, const char *message)
{

    int sock;
    struct sockaddr_in server;
    char server_reply[512];
    int status = -1;

    //Create socket
    sock = socket(AF_INET , SOCK_STREAM , 0);
    if (sock == -1)
    {
	//PL_DEBUG( "Could not create socket" );
        return -1;
    }

    server.sin_addr.s_addr = inet_addr(ip);
    server.sin_family = AF_INET;
    server.sin_port = htons( port );

    //Connect to remote server
    if (connect(sock , (struct sockaddr *)&server , sizeof(server)) < 0)
    {
        //PL_DEBUG("Connection to port 5000 failed. Could not send EndOfStream event");
        close(sock);
        return status;
    }

    if( send(sock , message , strlen(message) , 0) < 0)
    {
        puts("Send failed");
        close(sock);
        return status;
    }

    if( recv(sock , server_reply , 512 , 0) < 0)
    {
        puts("recv failed");
        close(sock);
        return status;
    }

    if (strncmp(server_reply, "SRV_Event_Server OK", 19) == 0)
       status = 0;

    close(sock);
    return status;

}


