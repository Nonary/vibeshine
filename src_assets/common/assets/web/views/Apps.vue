<template>
  <div class="container">
    <div class="my-4">
      <h1>{{ $t("apps.applications_title") }}</h1>
      <div>{{ $t("apps.applications_desc") }}</div>
    </div>
    <div class="card p-4">
      <table class="table">
        <thead>
          <tr>
            <th>{{ $t("apps.name") }}</th>
            <th>{{ $t("apps.actions") }}</th>
          </tr>
        </thead>
        <tbody>
          <tr v-for="(app, i) in apps" :key="i">
            <td>{{ app.name }}</td>
            <td>
              <button class="btn btn-primary mx-1" @click="editApp(i)">
                <font-awesome-icon icon="edit" /> {{ $t("apps.edit") }}
              </button>
              <button class="btn btn-danger mx-1" @click="showDeleteForm(i)">
                <font-awesome-icon icon="trash" /> {{ $t("apps.delete") }}
              </button>
            </td>
          </tr>
        </tbody>
      </table>
    </div>
    <div class="edit-form card mt-2" v-if="showEditForm">
      <div class="p-4">
        <div class="mb-3">
          <label class="form-label">{{ $t("apps.app_name") }}</label
          ><input v-model="editForm.name" type="text" class="form-control" />
        </div>
        <div class="mb-3">
          <label class="form-label">{{ $t("apps.output_name") }}</label
          ><input
            v-model="editForm.output"
            type="text"
            class="form-control monospace"
          />
        </div>
        <Checkbox
          class="mb-3"
          id="excludeGlobalPrep"
          label="apps.global_prep_name"
          desc="apps.global_prep_desc"
          v-model="editForm['exclude-global-prep-cmd']"
          default="true"
          inverse-values
        />
        <div class="mb-3">
          <label class="form-label">{{ $t("apps.cmd_prep_name") }}</label>
          <div class="form-text">{{ $t("apps.cmd_prep_desc") }}</div>
          <div
            class="d-flex justify-content-start mb-3 mt-3"
            v-if="editForm['prep-cmd'].length === 0"
          >
            <button class="btn btn-success" @click="addPrepCmd">
              <font-awesome-icon icon="plus" class="mr-1" />
              {{ $t("apps.add_cmds") }}
            </button>
          </div>
          <table class="table" v-if="editForm['prep-cmd'].length > 0">
            <thead>
              <tr>
                <th>
                  <font-awesome-icon icon="play" /> {{ $t("_common.do_cmd") }}
                </th>
                <th>
                  <font-awesome-icon icon="undo" /> {{ $t("_common.undo_cmd") }}
                </th>
                <th v-if="platform === 'windows'">
                  <font-awesome-icon icon="shield-alt" />
                  {{ $t("_common.run_as") }}
                </th>
                <th></th>
              </tr>
            </thead>
            <tbody>
              <tr v-for="(c, i) in editForm['prep-cmd']" :key="'pc' + i">
                <td>
                  <input
                    v-model="c.do"
                    type="text"
                    class="form-control monospace"
                  />
                </td>
                <td>
                  <input
                    v-model="c.undo"
                    type="text"
                    class="form-control monospace"
                  />
                </td>
                <td v-if="platform === 'windows'" class="align-middle">
                  <Checkbox
                    :id="'prep-cmd-admin-' + i"
                    label="_common.elevated"
                    desc=""
                    v-model="c.elevated"
                  />
                </td>
                <td>
                  <button
                    class="btn btn-danger"
                    @click="editForm['prep-cmd'].splice(i, 1)"
                  >
                    <font-awesome-icon icon="trash" /></button
                  ><button class="btn btn-success" @click="addPrepCmd">
                    <font-awesome-icon icon="plus" />
                  </button>
                </td>
              </tr>
            </tbody>
          </table>
        </div>
        <div class="mb-3">
          <label class="form-label">{{ $t("apps.detached_cmds") }}</label>
          <div
            v-for="(c, i) in editForm.detached"
            class="d-flex my-2"
            :key="'dc' + i"
          >
            <input
              v-model="editForm.detached[i]"
              type="text"
              class="form-control monospace"
            /><button
              class="btn btn-danger mx-2"
              @click="editForm.detached.splice(i, 1)"
            >
              &times;
            </button>
          </div>
          <div>
            <button class="btn btn-success" @click="editForm.detached.push('')">
              <font-awesome-icon icon="plus" class="mr-1" />
              {{ $t("apps.detached_cmds_add") }}
            </button>
          </div>
          <div class="form-text">
            <b>{{ $t("_common.note") }}</b> {{ $t("apps.detached_cmds_note") }}
          </div>
        </div>
        <div class="mb-3">
          <label class="form-label">{{ $t("apps.cmd") }}</label
          ><input
            v-model="editForm.cmd"
            type="text"
            class="form-control monospace"
          />
        </div>
        <div class="mb-3">
          <label class="form-label">{{ $t("apps.working_dir") }}</label
          ><input
            v-model="editForm['working-dir']"
            type="text"
            class="form-control monospace"
          />
        </div>
        <Checkbox
          v-if="platform === 'windows'"
          class="mb-3"
          id="appElevation"
          label="_common.run_as"
          desc="apps.run_as_desc"
          v-model="editForm.elevated"
          default="false"
        />
        <Checkbox
          class="mb-3"
          id="autoDetach"
          label="apps.auto_detach"
          desc="apps.auto_detach_desc"
          v-model="editForm['auto-detach']"
          default="true"
        />
        <Checkbox
          class="mb-3"
          id="waitAll"
          label="apps.wait_all"
          desc="apps.wait_all_desc"
          v-model="editForm['wait-all']"
          default="true"
        />
        <div class="mb-3">
          <label class="form-label">{{ $t("apps.exit_timeout") }}</label
          ><input
            v-model.number="editForm['exit-timeout']"
            type="number"
            class="form-control monospace"
            min="0"
            placeholder="5"
          />
        </div>
        <div class="mb-3">
          <label class="form-label">{{ $t("apps.image") }}</label
          ><input
            v-model="editForm['image-path']"
            type="text"
            class="form-control monospace"
          />
        </div>
        <div class="d-flex">
          <button class="btn btn-secondary m-2" @click="showEditForm = false">
            {{ $t("_common.cancel") }}</button
          ><button class="btn btn-primary m-2" @click="save">
            {{ $t("_common.save") }}
          </button>
        </div>
      </div>
    </div>
    <div class="mt-2" v-else>
      <button class="btn btn-primary" @click="newApp">
        <font-awesome-icon icon="plus" /> {{ $t("apps.add_new") }}
      </button>
    </div>
  </div>
</template>
<script>
import Checkbox from "@/Checkbox.vue";
export default {
  components: { Checkbox },
  data() {
    return { apps: [], showEditForm: false, editForm: null, platform: "" };
  },
  created() {
    fetch("./api/apps")
      .then((r) => r.json())
      .then((r) => {
        this.apps = r.apps;
      });
    fetch("./api/config")
      .then((r) => r.json())
      .then((r) => (this.platform = r.platform));
  },
  methods: {
    newApp() {
      this.editForm = {
        name: "",
        output: "",
        cmd: [],
        index: -1,
        "exclude-global-prep-cmd": false,
        elevated: false,
        "auto-detach": true,
        "wait-all": true,
        "exit-timeout": 5,
        "prep-cmd": [],
        detached: [],
        "image-path": "",
      };
      this.showEditForm = true;
    },
    editApp(id) {
      this.editForm = JSON.parse(JSON.stringify(this.apps[id]));
      this.editForm.index = id;
      ["prep-cmd", "detached"].forEach((k) => {
        if (!this.editForm[k]) this.editForm[k] = [];
      });
      if (this.editForm["exclude-global-prep-cmd"] === undefined)
        this.editForm["exclude-global-prep-cmd"] = false;
      ["elevated", "auto-detach", "wait-all", "exit-timeout"].forEach((k) => {
        if (this.editForm[k] === undefined) {
          this.editForm[k] = k === "exit-timeout" ? 5 : true;
        }
      });
      this.showEditForm = true;
    },
    showDeleteForm(id) {
      if (confirm("Are you sure to delete " + this.apps[id].name + "?")) {
        fetch("./api/apps/" + id, {
          method: "DELETE",
          headers: { "Content-Type": "application/json" },
        }).then((r) => {
          if (r.status === 200) location.reload();
        });
      }
    },
    addPrepCmd() {
      let t = { do: "", undo: "" };
      if (this.platform === "windows") t = { ...t, elevated: false };
      this.editForm["prep-cmd"].push(t);
    },
    save() {
      this.editForm["image-path"] = this.editForm["image-path"]
        .toString()
        .replace(/"/g, "");
      fetch("./api/apps", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(this.editForm),
      }).then((r) => {
        if (r.status === 200) location.reload();
      });
    },
  },
};
</script>
