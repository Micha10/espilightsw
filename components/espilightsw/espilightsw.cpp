#include "esphome/core/log.h"
#include <ESPiLight.h>
#include "espilightsw.h"

namespace esphome {
namespace espilightsw {

static const char *const TAG = "espilightsw_switch";

void EspilightComponent::setup() {
  ESP_LOGCONFIG(TAG, "Espilight '%s'...", this->get_name().c_str());

//   ESP_LOGD(TAG, "  Added callback for temperature '%s'", this->temperature_sensor_->get_name().c_str());
//   this->temperature_sensor_->add_on_state_callback([this](float state) { this->temperature_callback_(state); });
//   if (this->temperature_sensor_->has_state()) {
//     this->temperature_callback_(this->temperature_sensor_->get_state());
//   }

//   ESP_LOGD(TAG, "  Added callback for relative humidity '%s'", this->humidity_sensor_->get_name().c_str());
//   this->humidity_sensor_->add_on_state_callback([this](float state) { this->humidity_callback_(state); });
//   if (this->humidity_sensor_->has_state()) {
//     this->humidity_callback_(this->humidity_sensor_->get_state());
//   }
}

void EspilightComponent::dump_config() {
    LOG_SWITCH("", "Espilight Switch", this); 
}

void EspilightComponent::write_state(bool state) {
  if (state) {
    source_->turn_on();
    do_it();
  } else {
    source_->turn_off();
  }
}

void EspilightComponent::do_it(void) {
    if (pespilight == nullptr) {
        pespilight = new ESPiLight(pin_);
    }
    
    pespilight->send(this->protocol_name_, this->protocol_data_);    
}

float EspilightComponent::get_setup_priority() const { return setup_priority::DATA; }

// void EspilightComponent::loop() {
//   if (!this->next_update_) {
//     return;
//   }
//   this->next_update_ = false;

//   // Ensure we have source data
//   const bool no_temperature = std::isnan(this->temperature_);
//   const bool no_humidity = std::isnan(this->humidity_);
//   if (no_temperature || no_humidity) {
//     if (no_temperature) {
//       ESP_LOGW(TAG, "No valid state from temperature sensor!");
//     }
//     if (no_humidity) {
//       ESP_LOGW(TAG, "No valid state from temperature sensor!");
//     }
//     ESP_LOGW(TAG, "Unable to calculate absolute humidity.");
//     this->publish_state(NAN);
//     this->status_set_warning();
//     return;
//   }

  // Convert to desired units
//   const float temperature_c = this->temperature_;
//   const float temperature_k = temperature_c + 273.15;
//   const float hr = this->humidity_ / 100;

//   // Calculate saturation vapor pressure
//   float es;
//   switch (this->equation_) {
//     case BUCK:
//       es = es_buck(temperature_c);
//       break;
//     case TETENS:
//       es = es_tetens(temperature_c);
//       break;
//     case WOBUS:
//       es = es_wobus(temperature_c);
//       break;
//     default:
//       ESP_LOGE(TAG, "Invalid saturation vapor pressure equation selection!");
//       this->publish_state(NAN);
//       this->status_set_error();
//       return;
//   }
//   ESP_LOGD(TAG, "Saturation vapor pressure %f kPa", es);

//   // Calculate absolute humidity
//   const float absolute_humidity = vapor_density(es, hr, temperature_k);

//   // Publish absolute humidity
//   ESP_LOGD(TAG, "Publishing absolute humidity %f g/m³", absolute_humidity);
//   this->status_clear_warning();
//   this->publish_state(absolute_humidity);

}  // namespace absolute_humidity
}  // namespace esphome
