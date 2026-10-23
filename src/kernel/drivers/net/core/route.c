#include <drivers/net/core/route.h>

static struct dlist routes4;
static struct dlist routes6;

void route_initialize()
{
    dlist_init(&routes4);
    dlist_init(&routes6);
}
