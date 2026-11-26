# Firma de código con SignPath Foundation

BatteryHub ya está preparado para firmarse gratis con [SignPath Foundation](https://signpath.org)
(certificado de código abierto). Lo que falta lo hace el dueño del repo, porque necesita su cuenta.

## Qué ya está hecho en el repo

- **Licencia MIT** (`LICENSE`): SignPath Foundation solo firma proyectos con licencia de código abierto.
- **Política de firma** en el `README.md` (sección *Code signing policy*, con roles, política de privacidad, cambios en el sistema y desinstalación; cada release la enlaza al pie): SignPath exige que esté en la
  página del proyecto.
- **El exe lleva nombre y versión** (`assets/app.rc`: ProductName, FileVersion, CompanyName,
  LegalCopyright): SignPath comprueba esos metadatos al firmar.
- **Compilación en GitHub Actions** (`.github/workflows/release.yml`): sube el exe sin firmar como
  artefacto `BatteryHub-unsigned` y, si SignPath está configurado, lo manda a firmar y publica el
  firmado. Si no está configurado, publica sin firmar igual que antes.
- **Configuración del artefacto** lista para pegar: `docs/signpath/artifact-configuration.xml`.

## Pasos (una sola vez)

1. **Activá la verificación en dos pasos (2FA) en GitHub** si no la tenés: es requisito.
2. **Pedí el alta** en <https://signpath.org/apply> con el repo
   `https://github.com/Maurito2004/BatteryHub`. Te responden por mail (puede tardar días o semanas).
3. Cuando te aprueben, en <https://app.signpath.io>:
   - Instalá la app de GitHub de SignPath sobre este repo (*Trusted Build Systems → GitHub.com*).
   - En el proyecto (slug `BatteryHub`), pegá `docs/signpath/artifact-configuration.xml` como
     *artifact configuration*.
   - Anotá el **Organization ID** y el slug de la política de firma (`release-signing`).
   - Creá un usuario de CI (*Users → CI user*) y copiá su **API token**.
4. En GitHub → *Settings → Secrets and variables → Actions*:
   - **Secret** `SIGNPATH_API_TOKEN` = el token del paso 3.
   - **Variable** `SIGNPATH_ORGANIZATION_ID` = el Organization ID.
   - (Opcional) variables `SIGNPATH_PROJECT_SLUG` y `SIGNPATH_POLICY_SLUG` si usaste otros nombres.
5. Publicá una versión como siempre (`git tag -a vX.Y.Z ... && git push origin vX.Y.Z`). Con la
   política `release-signing` SignPath te pide aprobar cada firma desde su web o el mail.

## Comprobar que salió firmado

```powershell
Get-AuthenticodeSignature .\BatteryHub.exe | Format-List Status, SignerCertificate
```

`Status : Valid` y el certificado a nombre de *SignPath Foundation* = firmado.
