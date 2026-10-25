#ifndef SUBSCRIBER_CLASS_H
#define SUBSCRIBER_CLASS_H

#include "../traverse.h"

/* Return a subscriber that reacts to ClassDef nodes. */
AstSubscriber *class_subscriber(void);

#endif /* SUBSCRIBER_CLASS_H */
