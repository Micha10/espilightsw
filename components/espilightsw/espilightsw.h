#pragma once
#ifndef _ESPILIGHTSW_INCLUDE
#define _ESPILIGHTSW_INCLUDE

#include "esphome/core/component.h"
#include "esphome/components/switch/switch.h"

namespace esphome {
    namespace espilightsw {

        static const char *const TAG = "espilightsw_switch";

#include <ESPiLight.h>


/// This class implements calculation of absolute humidity from temperature and relative humidity.
        class EspilightComponent : public Switch, public Component {
        public:
            EspilightComponent() = default;

            void set_protocol_name(String protocol_name) { this->protocol_name_ = protocol_name; }

            void set_protocol_data(String protocol_data) { this->protocol_data_ = protocol_data; }

            void set_pin(int pin) { pin_ = pin; }

            void dump_config() {
                LOG_SWITCH(TAG, "Espilight name", this->protocol_name_);
                LOG_SWITCH(TAG, "Espilight data", this->protocol_data_);
                LOG_PIN("Pin:", this->pin_);
            }

            void setup() {
                ESP_LOGI(TAG, "Initializing");
            }

            float get_setup_priority() const override;

            switch_::Switch *source_{nullptr};
            ESPiLight *pespilight{nullptr};


        protected:
            void do_it(void) {
                if (pespilight == nullptr) {
                    pespilight = new ESPiLight(pin_);
                }
                pespilight->send(this->protocol_name_, this->protocol_data_);
            }

            float get_setup_priority() const { return setup_priority::DATA; }

            void write_state(bool state) {
                if (state) {
                    source_->turn_on();
                    do_it();
                } else {
                    source_->turn_off();
                }
            }

            String protocol_name_ = "";
            String protocol_data_ = "";

            int pin_ = 0;
        };

    }  // namespace espilightsw
}  // namespace esphome

#endif
