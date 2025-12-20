# Bazel Central Registry (BCR) Publishing

This directory contains template files for automated publishing to the [Bazel Central Registry](https://github.com/bazelbuild/bazel-central-registry).

## Overview

The workflow automatically publishes new versions of gperftools to the BCR when a new tag is created following the pattern `gperftools-X.Y.Z`.

## Setup

### 1. Create a BCR Fork

The maintainer needs to fork the [Bazel Central Registry](https://github.com/bazelbuild/bazel-central-registry) repository to their personal or organization account.

### 2. Create a Personal Access Token (PAT)

1. Go to GitHub Settings → Developer settings → Personal access tokens → Tokens (classic)
2. Click "Generate new token (classic)"
3. Give it a descriptive name (e.g., "BCR Publish Token")
4. Select the following scopes:
   - `repo` (full control of private repositories)
   - `workflow` (update GitHub Action workflows)
5. Generate the token and copy it immediately (it won't be shown again)

### 3. Add the Token as a Repository Secret

1. Go to the gperftools repository settings
2. Navigate to Secrets and variables → Actions
3. Click "New repository secret"
4. Name: `BCR_PUBLISH_TOKEN`
5. Value: Paste the PAT you created
6. Click "Add secret"

## Usage

### Automatic Publishing

When you create a new release tag (e.g., `gperftools-2.17.3`), the workflow will automatically:

1. Trigger on the tag push
2. Generate the BCR entry using the template files in this directory
3. Push the entry to your BCR fork
4. Open a draft pull request to the official BCR

### Manual Publishing

If you need to republish or troubleshoot, you can manually trigger the workflow:

1. Go to Actions → Publish to BCR
2. Click "Run workflow"
3. Enter the tag name (e.g., `gperftools-2.17.3`)
4. Click "Run workflow"

## Template Files

- **metadata.template.json**: Contains repository information and maintainers
- **source.template.json**: Defines the source archive URL pattern and structure
- **presubmit.yml**: Specifies the build and test targets for BCR validation

These files are used to generate the actual BCR entry when a new version is published.

## Troubleshooting

### Pull Request Not Created

- Verify that `BCR_PUBLISH_TOKEN` is set correctly in repository secrets
- Check that the PAT has `repo` and `workflow` permissions
- Ensure the BCR fork exists and is accessible

### Build Failures in BCR

- Review the presubmit.yml targets to ensure they are correct
- Check that the MODULE.bazel file is up to date
- Verify the source archive URL pattern in source.template.json

## More Information

- [Publish to BCR Documentation](https://github.com/bazel-contrib/publish-to-bcr)
- [BCR Contribution Guidelines](https://github.com/bazelbuild/bazel-central-registry/blob/main/docs/README.md)
- [Bzlmod User Guide](https://bazel.build/docs/bzlmod)
