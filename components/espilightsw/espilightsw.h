#pragma once
#ifndef _ESPILIGHTSW_INCLUDE
#define _ESPILIGHTSW_INCLUDE

#include "esphome.h"
#include "esphome/core/component.h"
//#include "esphome/components/switch/switch.h"
//#include "ESPiLight/src/ESPiLight.h"
#include <ESPiLight.h>

namespace esphome {
namespace espilightsw {


/// This class implements calculation of absolute humidity from temperature and relative humidity.
class EspilightComponent : public Switch, public Component {
 public:
  EspilightComponent() = default;

  void set_protocol_name(String protocol_name) { this->protocol_name_ = protocol_name; }
  void set_protocol_data(String protocol_data) { this->protocol_data_ = protocol_data; }
  void set_pin(int pin) { pin_ = pin; }


  void setup() override;
  void dump_config() override;
  float get_setup_priority() const override;
  //void loop() override;
  switch_::Switch *source_{nullptr};

  ESPiLight *pespilight{nullptr};

 protected:
  void do_it();
  void write_state(bool state) override;

  String protocol_name_ = "";
  String protocol_data_ = "";
  
  int pin_ = 0;
};

}  // namespace espilightsw
}  // namespace esphome

#endif
