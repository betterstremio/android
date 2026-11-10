/* "Who's watching?": a Nuvio account's profiles and a locked profile's PIN, over whatever is on screen. */
#ifndef SB_PROFILES_H
#define SB_PROFILES_H
enum { PF_START, PF_SIGNIN, PF_SWITCH }; /* at launch (Home ready behind it), right after signing in, from the top bar or Settings */
void profiles_open(int mode);
int  profiles_is_open(void);
#endif
