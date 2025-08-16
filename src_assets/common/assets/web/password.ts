import { createApp } from 'vue';
import { initApp } from '@/init';
// import Navbar from '@/Navbar.vue'; // Legacy - not needed

interface PasswordData {
  currentUsername: string;
  currentPassword: string;
  newUsername: string;
  newPassword: string;
  confirmNewPassword: string;
}

interface PasswordResponse {
  status: boolean;
  error?: string;
}

const app = createApp({
  components: {
    Navbar,
  },
  data() {
    return {
      error: null as string | null,
      success: false,
      passwordData: {
        currentUsername: '',
        currentPassword: '',
        newUsername: '',
        newPassword: '',
        confirmNewPassword: '',
      } as PasswordData,
    };
  },
  methods: {
    save(): void {
      this.error = null;
      fetch('./api/password', {
        method: 'POST',
        headers: {
          'Content-Type': 'application/json',
        },
        body: JSON.stringify(this.passwordData),
      }).then((r) => {
        if (r.status === 200) {
          r.json().then((rj: PasswordResponse) => {
            this.success = rj.status;
            if (this.success === true) {
              setTimeout(() => {
                document.location.reload();
              }, 5000);
            } else {
              this.error = rj.error || 'Unknown error';
            }
          });
        } else {
          this.error = 'Internal Server Error';
        }
      });
    },
  },
});

initApp(app);