#ifndef foodroidcardaccessoriesfoo
#define foodroidcardaccessoriesfoo

#include <pulsecore/card.h>

typedef struct card_accessories card_accessories;

card_accessories *card_accessories_init(pa_card *card);
void card_accessories_done(card_accessories *accessories);

#endif
