use zed_extension_api as zed;

struct LunsoExtension;

impl zed::Extension for LunsoExtension {
    fn new() -> Self {
        Self
    }

    fn language_server_command(
        &mut self,
        _language_server_id: &zed::LanguageServerId,
        worktree: &zed::Worktree,
    ) -> zed::Result<zed::Command> {
        let binary = worktree
            .which("lunso-lsp")
            .ok_or_else(|| "lunso-lsp not found in PATH".to_string())?;

        Ok(zed::Command {
            command: binary,
            args: Vec::new(),
            env: worktree.shell_env(),
        })
    }
}

zed::register_extension!(LunsoExtension);
