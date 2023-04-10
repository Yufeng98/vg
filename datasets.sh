#!/bin/bash

git clone --recursive https://github.com/yukiteruono/pbsim2.git

VG_DIR=`pwd`

cd $VG_DIR/pbsim2
./configure
make
cd $VG_DIR
cp $VG_DIR/pbsim2/src/pbsim .

mkdir -p $VG_DIR/datasets
cd $VG_DIR/datasets

wget -O GRCh38.fa https://ftp.1000genomes.ebi.ac.uk/vol1/ftp/technical/reference/GRCh38_reference_genome/GRCh38_full_analysis_set_plus_decoy_hla.fa

python3 ../preprocess_GRCh38.py GRCh38.fa
rm tmp

for i in {1..22}
do
wget -O chr$i.vcf.gz http://ftp.1000genomes.ebi.ac.uk/vol1/ftp/data_collections/1000_genomes_project/release/20190312_biallelic_SNV_and_INDEL/ALL.chr$i.shapeit2_integrated_snvindels_v2a_27022019.GRCh38.phased.vcf.gz &
done
wget -O chrX.vcf.gz http://ftp.1000genomes.ebi.ac.uk/vol1/ftp/data_collections/1000_genomes_project/release/20190312_biallelic_SNV_and_INDEL/ALL.chrX.shapeit2_integrated_snvindels_v2a_27022019.GRCh38.phased.vcf.gz &
wait

for i in {1..22}
do
tabix -p vcf -f chr$i.vcf.gz &
done
tabix -p vcf -f chrX.vcf.gz &
wait

for i in {1..22}
do
$VG_DIR/bin/vg construct -r chr$i.fa -v chr$i.vcf.gz > chr$i.vg &
done
$VG_DIR/bin/vg construct -r chrX.fa -v chrX.vcf.gz > chrX.vg &
wait

# --difference-ratio 6:50:54 --hmm_model data/P6C4.model (PacBio)
# --difference-ratio 23:31:46 --hmm_model data/R94.model (Nanopore)
# --length-min 10000 --length-max 10000
# --accuracy-mean 0.90 or 0.95


$VG_DIR/pbsim --difference-ratio 6:50:54 --hmm_model $VG_DIR/pbsim2/data/P6C4.model --length-min 10000 --length-max 10000 --accuracy-mean 0.90 --prefix GRCh38_pacbio_10_error chr22.fa &
$VG_DIR/pbsim --difference-ratio 6:50:54 --hmm_model $VG_DIR/pbsim2/data/P6C4.model --length-min 10000 --length-max 10000 --accuracy-mean 0.95 --prefix GRCh38_pacbio_5_error chr22.fa &
$VG_DIR/pbsim --difference-ratio 23:31:46 --hmm_model $VG_DIR/pbsim2/data/R94.model --length-min 10000 --length-max 10000 --accuracy-mean 0.90 --prefix GRCh38_nanopore_10_error chr22.fa &
$VG_DIR/pbsim --difference-ratio 23:31:46 --hmm_model $VG_DIR/pbsim2/data/R94.model --length-min 10000 --length-max 10000 --accuracy-mean 0.95 --prefix GRCh38_nanopore_5_error chr22.fa &
wait

cd $VG_DIR


