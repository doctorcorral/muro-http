/* Pin decision_tags.h to the emitted decide constructors. */

#include "tag_pin.h"

#include <stdio.h>

int main(void) {
  if (!emitted_tags_match()) return 1;
  printf("OK\n");
  return 0;
}
