#ifndef __assassin_hh
#define __assassin_hh

namespace otk {

struct PointerAssassin {
  template<typename T>
  inline void operator()(const T ptr) const {
    delete ptr;
  }
};

}

#endif // __assassin_hh
