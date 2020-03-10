/****************** main.c for model {{ model_uuid }} generated by nn-loader.py *******************/
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/version.h>
#include <linux/slab.h>

#include "linux/liteflow.h"

{% for layer_id in range(0, layer_list|length) %}
/************************************** Layer {{ layer_id }} **************************************/
{{ layer_list[layer_id].generate_comp_code(layer_id) }}

{{ layer_list[layer_id].generate_struct_code(layer_id) }}
/************************************ End Layer {{ layer_id }} ************************************/
{% endfor %}

/************************************** Model {{ model_id }} **************************************/

struct model_container model_{{ model_uuid }} __read_mostly = {
    .uuid = {{ model_uuid }},
    .input_size = {{ input_size }},
    .output_size = {{ output_size }},
};

{% if test_mode %}
/******************************************** Test Mode ********************************************/
struct app app = {
    .appid = {{ app_id }},
    .input_size = {{ input_size }},
    .output_size = {{ output_size }},
};

/**************************************** End Test Mode ********************************************/
{% endif %}

static int
__init liteflow_{{ model_uuid }}_model_init(void)
{
    {% if test_mode %}
    s64 _input[{{ input_size }}];
    s64 _output[{{ output_size}}];
    int _output_pos;
    {% endif %}

    // Construct layers
    INIT_LIST_HEAD(&model_{{ model_uuid }}.layers);
    {% for layer_id in range(0, layer_list|length) %}
    {%- if loop.first -%}
    list_add(&layer_{{ layer_id }}.list, &model_{{ model_uuid }}.layers);
    {%- else %}
    list_add(&layer_{{ layer_id }}.list, &layer_{{ layer_id - 1}}.list);
    {%- endif -%}
    {% endfor %}

    {% if test_mode %}
    // Test mode = on
    lf_register_app(&app);
    {% endif %}
    
    lf_register_model({{ app_id }}, &model_{{ model_uuid }});

    {% if test_mode %}
    lf_activate_model({{ app_id }}, {{ model_uuid }});
    // TODO
    {% for input_pos in range(0, input_size) %}
    _input[{{ input_pos }}] = ... ;
    {%- endfor %}

    lf_query_model({{ app_id }}, _input, _output);

    for (_output_pos = 0; _output_pos < {{ output_size }}; ++_output_pos) {
        printk(KERN_INFO "Output_%d: %lld\n", _output_pos, _output[_output_pos]);
    }
    {% endif %}

    return 0;
}

static void
__exit liteflow_{{ model_uuid }}_model_exit(void)
{
    lf_unregister_model({{ app_id}}, {{ model_uuid }});

    {% if test_mode %}
    // Test mode = on
    lf_unregister_app({{ app_id }});
    {% endif %}
}

module_init(liteflow_{{ model_uuid }}_model_init);
module_exit(liteflow_{{ model_uuid }}_model_exit);

MODULE_DESCRIPTION("liteflow {{ model_uuid }} model");
MODULE_AUTHOR("liteflow");
MODULE_LICENSE("GPL v2");

/************************************ End Model {{ model_id }} ************************************/
