import esphome.config_validation as cv
import esphome.codegen as cg
from esphome.components import switch
from esphome import pins
from esphome.const import CONF_ID, CONF_PIN

#DEPENDENCIES = ["espilight"]

CONF_PROTOCOL_NAME = "protocol"
CONF_PROTOCOL_DATA = "data"

espilight_ns = cg.esphome_ns.namespace("espilightsw")
EspilightComponent = espilight_ns.class_(
    "EspilightComponent", switch.Switch, cg.Component
)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(EspilightComponent),
        #cv.Required(CONF_PIN): pins.internal_gpio_output_pin_schema,
        cv.Required(CONF_PIN): int,
        cv.Required(CONF_PROTOCOL_NAME): cv.string,
        cv.Required(CONF_PROTOCOL_DATA): cv.string,
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    # pin = await cg.gpio_pin_expression(config[CONF_PIN])
    # cg.add(var.set_pin(pin))
    cg.add(var.set_pin(config[CONF_PIN]))
    cg.add(var.set_protocol_name(config[CONF_PROTOCOL_NAME]))
    cg.add(var.set_protocol_data(config[CONF_PROTOCOL_DATA]))
    cg.add_library("https://github.com/Micha10/ESPiLight.git", None)

    # paren = await cg.get_variable(config[CONF_ID])
    # var = cg.new_Pvariable(config[CONF_ID], paren)
    # await cg.register_component(var, config)
    # pin = await cg.gpio_pin_expression(config[CONF_PIN])
    # cg.add(var.set_pin(pin))
    # cg.add(var.set_protocol_name(config[CONF_PROTOCOL_NAME]))
    # cg.add(var.set_protocol_data(config[CONF_PROTOCOL_DATA]))

    # cg.add(paren.register_sensor(var))
    #cg.add_library("puuu/espilight", "0.17.0")
