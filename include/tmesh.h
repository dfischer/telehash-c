#ifndef tmesh_h
#define tmesh_h

#include "mesh.h"

typedef struct tmesh_struct *tmesh_t;
typedef struct cmnty_struct *cmnty_t;
typedef struct epoch_struct *epoch_t;
typedef struct mote_struct *mote_t;
typedef struct medium_struct *medium_t;
typedef struct radio_struct *radio_t;
typedef struct knock_struct *knock_t;

// medium management w/ device driver
struct medium_struct
{
  uint8_t bin[5];
  void *device; // used by radio device driver
  uint32_t min, max; // microseconds to tx/rx, set by driver
  uint8_t chans; // number of total channels, set by driver
  uint8_t radio:4; // radio device id based on radio_devices[]
};

// validate medium by checking energy
uint32_t medium_check(tmesh_t tm, uint8_t medium[5]);

// get the full medium
medium_t medium_get(tmesh_t tm, uint8_t medium[5]);


// community management
struct cmnty_struct
{
  tmesh_t tm;
  char *name;
  medium_t medium;
  mote_t sync; // ping+echo, only pub/priv
  mote_t motes; 
  pipe_t pipe; // one pipe per community as it's shared performance
  struct cmnty_struct *next;
  uint8_t max;
  enum {PUBLIC, PRIVATE} type:1;
};

// maximum neighbors tracked per community
#define NEIGHBORS_MAX 8

// join a new private/public community
cmnty_t tmesh_join(tmesh_t tm, char *medium, char *name);

// add a link known to be in this community to look for
mote_t tmesh_link(tmesh_t tm, cmnty_t c, link_t link);

// overall tmesh manager
struct tmesh_struct
{
  mesh_t mesh;
  cmnty_t coms;
  lob_t pubim;
  uint8_t z; // our z-index
  // TODO, add callback hooks for sorting/prioritizing energy usage
};

// create a new tmesh radio network bound to this mesh
tmesh_t tmesh_new(mesh_t mesh, lob_t options);
void tmesh_free(tmesh_t tm);

// process any full packets into the mesh 
tmesh_t tmesh_loop(tmesh_t tm);

// a single knock request ready to go
struct knock_struct
{
  cmnty_t com; // has medium
  mote_t mote; // has chunks
  epoch_t epoch; // details
  // ephemeral things
  uint32_t win;
  uint64_t start, stop; // microsecond exact start/stop time
  uint8_t chan; // current channel (< med->chans)
};

// fills in next knock based on from and only for this device
tmesh_t tmesh_knock(tmesh_t tm, knock_t k, uint64_t from, radio_t device);
tmesh_t tmesh_knocking(tmesh_t tm, knock_t k, uint8_t *frame); // prep for tx
tmesh_t tmesh_knocked(tmesh_t tm, knock_t k, uint8_t *frame); // process done knock


// 2^22
#define EPOCH_WINDOW (uint64_t)4194304

// mote state tracking
struct mote_struct
{
  link_t link; // when known
  epoch_t epochs;
  mote_t next; // for lists
  uint8_t ping; // anytime we transmit on this channel, reschedule the echo epoch
  util_chunks_t chunks; // actual chunk encoding for r/w frame buffers

  uint8_t z;
};

mote_t mote_new(link_t link);
mote_t mote_free(mote_t m);

// find best epoch, set knock win/chan/start/stop
mote_t mote_knock(mote_t m, knock_t k, uint64_t from);

// individual epoch state data
struct epoch_struct
{
  uint8_t secret[32];
  uint64_t base; // microsecond of window 0 start
  epoch_t next; // for epochs_* list utils
  enum {TX, RX} dir:1;
  enum {RESET, PING, ECHO, PAIR, LINK} type:4;

};

epoch_t epoch_new(uint8_t tx);
epoch_t epoch_free(epoch_t e);

// sync point for given window
epoch_t epoch_base(epoch_t e, uint32_t window, uint64_t at);

// simple array utilities
epoch_t epochs_add(epoch_t es, epoch_t e);
epoch_t epochs_rem(epoch_t es, epoch_t e);
size_t epochs_len(epoch_t es);
epoch_t epochs_free(epoch_t es);

///////////////////
// radio devices are responsible for all mediums
struct radio_struct
{
  uint8_t id;

  // return energy cost, or 0 if unknown medium, use for pre-validation/estimation
  uint32_t (*energy)(tmesh_t tm, uint8_t medium[5]);

  // initialize/get any medium scheduling time/cost and channels
  medium_t (*get)(tmesh_t tm, uint8_t medium[5]);

  // when a medium isn't used anymore, let the radio free it
  medium_t (*free)(tmesh_t tm, medium_t m);

};

#define RADIOS_MAX 1
extern radio_t radio_devices[]; // all of em

// add/set a new device
radio_t radio_device(radio_t device);



#endif
