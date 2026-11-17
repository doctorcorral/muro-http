#ifndef MURO_HTTP_TAG_PIN_H
#define MURO_HTTP_TAG_PIN_H

/* 1 when each stub tag's emitted constructor matches decision_tags.h.
   0 otherwise. The _Static_asserts in test_render.c do not call the
   emitter. This does, through the generated *From helpers. */
int emitted_tags_match(void);

#endif
