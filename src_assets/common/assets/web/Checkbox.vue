<script setup>
import { computed } from 'vue';
import { NCheckbox } from 'naive-ui';
const model = defineModel({ required: true });
const slots = defineSlots();
const props = defineProps({
  class: {
    type: String,
    default: '',
  },
  disabled: {
    type: Boolean,
    default: false,
  },
  desc: {
    type: String,
    default: null,
  },
  id: {
    type: String,
    required: true,
  },
  label: {
    type: String,
    default: null,
  },
  localePrefix: {
    type: String,
    default: 'missing-prefix',
  },
  inverseValues: {
    type: Boolean,
    default: false,
  },
  // renamed from `default` to avoid ambiguity with slot name
  defaultValue: {
    type: undefined,
    default: null,
  },
});

// Add the mandatory class values
const extendedClassStr = (() => {
  let values = props.class.split(' ');
  if (!values.includes('form-check')) {
    values.push('form-check');
  }
  return values.join(' ');
})();

// Map the value to boolean representation if possible, otherwise return null.
// Accepts undefined/null and returns null so caller can decide fallback behavior.
const mapToBoolRepresentation = (value) => {
  // Try literal values first
  if (value === true || value === false) {
    return { possibleValues: [true, false], value: value };
  }
  if (value === 1 || value === 0) {
    return { possibleValues: [1, 0], value: value };
  }

  const stringPairs = [
    ['true', 'false'],
    ['1', '0'],
    ['enabled', 'disabled'],
    ['enable', 'disable'],
    ['yes', 'no'],
    ['on', 'off'],
  ];

  value = `${value}`.toLowerCase().trim();
  for (const pair of stringPairs) {
    if (value === pair[0] || value === pair[1]) {
      return { possibleValues: pair, value: value };
    }
  }

  return null;
};

// Determine the true/false values for the checkbox reactively so that
// if the bound value type changes (e.g., server returns boolean later),
// the mapping updates to keep the checked state in sync.
const checkboxValues = computed(() => {
  const mappedValues = (() => {
    let boolValues = mapToBoolRepresentation(model.value);
    if (boolValues !== null) return boolValues.possibleValues;
    if (model.value === undefined || model.value === null) {
      const defaultParsed = mapToBoolRepresentation(props.defaultValue);
      if (defaultParsed !== null) return defaultParsed.possibleValues;
    }
    return ['true', 'false'];
  })();
  const truthyIndex = props.inverseValues ? 1 : 0;
  const falsyIndex = props.inverseValues ? 0 : 1;
  return { truthy: mappedValues[truthyIndex], falsy: mappedValues[falsyIndex] };
});

const parsedDefaultPropValue = computed(() => {
  const boolValues = mapToBoolRepresentation(props.defaultValue);
  if (boolValues !== null) {
    return boolValues.value === boolValues.possibleValues[0];
  }
  return null;
});

const labelField = props.label ?? `${props.localePrefix}.${props.id}`;
const descField = props.desc ?? `${props.localePrefix}.${props.id}_desc`;
const showDesc = props.desc !== '' || Object.entries(slots).length > 0;
const showDefValue = computed(() => parsedDefaultPropValue.value !== null);
const defValue = computed(() =>
  parsedDefaultPropValue.value ? '_common.enabled_def_cbox' : '_common.disabled_def_cbox',
);
</script>

<template>
  <div :class="extendedClassStr">
    <n-checkbox
      :id="props.id"
      v-model:checked="model"
      :true-value="checkboxValues.truthy"
      :false-value="checkboxValues.falsy"
    >
      {{ $t(labelField) }}
    </n-checkbox>
    <div v-if="showDefValue" class="mt-0 form-text">
      {{ $t(defValue) }}
    </div>
    <div v-if="showDesc" class="form-text">
      {{ $t(descField) }}
      <slot />
    </div>
  </div>
</template>
