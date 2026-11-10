#ifndef pjl_config_h
#define pjl_config_h

#define PJL_DISCARD_RV(FN_CALL) \
  do { [[maybe_unused]] typeof(FN_CALL) _rv = (FN_CALL); } while (0)

#endif /* pjl_config_h */
